#include "PCH.h"
#include "TransformDataModule.h"
#include "BaseComponents.h"
#include "BufferManager.h"
#include "ObjectManager.h"

// Платформенный гейт SIMD. Сборка может выключить его сама (-DTDM_SIMD_X86=0); под платформу
// без x86-интринсиков модуль собирается со скалярными заготовками ниже.
#if !defined(TDM_SIMD_X86)
    #if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        #define TDM_SIMD_X86 1
    #else
        #define TDM_SIMD_X86 0
    #endif
#endif

#if TDM_SIMD_X86
    #include <immintrin.h>
#endif

// 16 float'ов GPU-матрицы, column-major: столбцы (x,y,z,i), (a,b,c,j), (e,f,g,k), (w,d,h,l).
// В этом же порядке GatherPositionStreams собирает потоки для транспонирования.
static void LoadPositionMatrix(const Positions& P, size_t i, float* m)
{
    m[0]  = P.x[i]; m[1]  = P.y[i]; m[2]  = P.z[i]; m[3]  = P.i[i];
    m[4]  = P.a[i]; m[5]  = P.b[i]; m[6]  = P.c[i]; m[7]  = P.j[i];
    m[8]  = P.e[i]; m[9]  = P.f[i]; m[10] = P.g[i]; m[11] = P.k[i];
    m[12] = P.w[i]; m[13] = P.d[i]; m[14] = P.h[i]; m[15] = P.l[i];
}

static void StorePositionMatrix(Positions& P, size_t i, const float* m)
{
    P.x[i] = m[0];  P.y[i] = m[1];  P.z[i] = m[2];  P.i[i] = m[3];
    P.a[i] = m[4];  P.b[i] = m[5];  P.c[i] = m[6];  P.j[i] = m[7];
    P.e[i] = m[8];  P.f[i] = m[9];  P.g[i] = m[10]; P.k[i] = m[11];
    P.w[i] = m[12]; P.d[i] = m[13]; P.h[i] = m[14]; P.l[i] = m[15];
}

static void LoadLocalMatrix(const LocalMatrices& L, size_t i, float* m)
{
    m[0]  = L.m0[i];  m[1]  = L.m1[i];  m[2]  = L.m2[i];  m[3]  = L.m3[i];
    m[4]  = L.m4[i];  m[5]  = L.m5[i];  m[6]  = L.m6[i];  m[7]  = L.m7[i];
    m[8]  = L.m8[i];  m[9]  = L.m9[i];  m[10] = L.m10[i]; m[11] = L.m11[i];
    m[12] = L.m12[i]; m[13] = L.m13[i]; m[14] = L.m14[i]; m[15] = L.m15[i];
}

// lhs = lhs * rhs, column-major. Все четыре столбца lhs снимаются ДО первой записи: каждый
// столбец результата — комбинация всех столбцов lhs, и запись нулевого затёрла бы данные,
// нужные остальным. Заготовка без SIMD делает то же самое через копию (замер: 3.8 мс против
// 2.2 на 200k матриц).
#if TDM_SIMD_X86
static inline void MulMat4InPlace(float* lhs, const float* rhs)
{
    const __m128 c0 = _mm_loadu_ps(lhs + 0);
    const __m128 c1 = _mm_loadu_ps(lhs + 4);
    const __m128 c2 = _mm_loadu_ps(lhs + 8);
    const __m128 c3 = _mm_loadu_ps(lhs + 12);

    for (int j = 0; j < 4; ++j) {
        const float* b = rhs + j * 4;
        __m128 col =          _mm_mul_ps(c0, _mm_set1_ps(b[0]));
        col = _mm_add_ps(col, _mm_mul_ps(c1, _mm_set1_ps(b[1])));
        col = _mm_add_ps(col, _mm_mul_ps(c2, _mm_set1_ps(b[2])));
        col = _mm_add_ps(col, _mm_mul_ps(c3, _mm_set1_ps(b[3])));
        _mm_storeu_ps(lhs + j * 4, col);
    }
}
#else
static inline void MulMat4InPlace(float* lhs, const float* rhs)
{
    float a[16];
    for (int k = 0; k < 16; ++k) a[k] = lhs[k];

    for (int j = 0; j < 4; ++j) {
        const float b0 = rhs[j * 4 + 0], b1 = rhs[j * 4 + 1];
        const float b2 = rhs[j * 4 + 2], b3 = rhs[j * 4 + 3];
        for (int i = 0; i < 4; ++i)
            lhs[j * 4 + i] = a[i] * b0 + a[4 + i] * b1 + a[8 + i] * b2 + a[12 + i] * b3;
    }
}
#endif

void TransformDataModule::UpdateLocalTransforms(ObjectManager* om, SceneData* scene)
{
    const uint64_t rev = om->EntityRevision();
    if (rev != links_revision_) {
        links_revision_ = rev;
        local_links_.clear();
        om->ForEach<Positions, ParentComponent, LocalMatrices>(scene,
            [&](SoAElement<Positions> pos_el, ParentComponent& parentComp, SoAElement<LocalMatrices> local_el)
        {
            auto arch_it = scene->entity_to_archetype.find(parentComp.parent);
            if (arch_it == scene->entity_to_archetype.end() || !arch_it->second) return;
            auto* parentPosArr = arch_it->second->get_array<Positions>();
            if (!parentPosArr) return;
            auto idx_it = scene->entity_to_index.find(parentComp.parent);
            if (idx_it == scene->entity_to_index.end()) return;

            local_links_.push_back(LocalXformLink{
                pos_el.soa, local_el.soa, &parentPosArr->data,
                pos_el.index, local_el.index, idx_it->second });
        });
    }

    for (const LocalXformLink& r : local_links_) {
        float world[16], local[16];
        LoadPositionMatrix(*r.parent_pos, r.parent_i, world);
        LoadLocalMatrix(*r.local, r.local_i, local);
        MulMat4InPlace(world, local);
        StorePositionMatrix(*r.child_pos, r.child_i, world);
    }
}

uint32_t TransformDataModule::CalculateTransformSize(ObjectManager* om, SceneData* scene)
{
    const uint64_t rev = om->EntityRevision();
    if (rev == size_revision_) {
        return total_size;
    }
    size_revision_ = rev;
    total_size = 0;

    om->ForEachArchetype<Positions, DrawComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr, ComponentArray<DrawComponent, void>*)
    {
        total_size += safe_u32(posArr->size()) * sizeof(PositionProxy16);
    });

    return total_size;
}

static void GatherPositionStreams(const Positions& P, const float* src[16])
{
    src[0]  = P.x.data(); src[1]  = P.y.data(); src[2]  = P.z.data(); src[3]  = P.i.data();
    src[4]  = P.a.data(); src[5]  = P.b.data(); src[6]  = P.c.data(); src[7]  = P.j.data();
    src[8]  = P.e.data(); src[9]  = P.f.data(); src[10] = P.g.data(); src[11] = P.k.data();
    src[12] = P.w.data(); src[13] = P.d.data(); src[14] = P.h.data(); src[15] = P.l.data();
}

static void TransposeScalar(const float* const src[16], float* dst, size_t first, size_t last)
{
    for (size_t e = first; e < last; ++e) {
        float* m = dst + e * 16;
        for (size_t s = 0; s < 16; ++s)
            m[s] = src[s][e];
    }
}

#if TDM_SIMD_X86
static inline void Transpose8x8(__m256 r[8])
{
    __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
    __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
    __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
    __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
    __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]);
    __m256 t5 = _mm256_unpackhi_ps(r[4], r[5]);
    __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]);
    __m256 t7 = _mm256_unpackhi_ps(r[6], r[7]);

    __m256 u0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));

    r[0] = _mm256_permute2f128_ps(u0, u4, 0x20);
    r[1] = _mm256_permute2f128_ps(u1, u5, 0x20);
    r[2] = _mm256_permute2f128_ps(u2, u6, 0x20);
    r[3] = _mm256_permute2f128_ps(u3, u7, 0x20);
    r[4] = _mm256_permute2f128_ps(u0, u4, 0x31);
    r[5] = _mm256_permute2f128_ps(u1, u5, 0x31);
    r[6] = _mm256_permute2f128_ps(u2, u6, 0x31);
    r[7] = _mm256_permute2f128_ps(u3, u7, 0x31);
}

// Укладывает блоками по 8 сущностей и возвращает, сколько уложено; остаток добирает вызывающий.
// Ручной AVX здесь не украшение, не векторизуется.
static size_t TransposeBlocks(const float* const src[16], float* dst, size_t n)
{
    size_t e = 0;
    for (; e + 8 <= n; e += 8) {
        __m256 lo[8], hi[8];
        for (int s = 0; s < 8; ++s) {
            lo[s] = _mm256_loadu_ps(src[s] + e);
            hi[s] = _mm256_loadu_ps(src[s + 8] + e);
        }
        Transpose8x8(lo);
        Transpose8x8(hi);

        float* out = dst + e * 16;
        for (int m = 0; m < 8; ++m) {
            _mm256_storeu_ps(out + m * 16,     lo[m]);
            _mm256_storeu_ps(out + m * 16 + 8, hi[m]);
        }
    }
    return e;
}
#else
// Заготовка: блоков нет, всё уедет в скалярный проход. Пустой она быть может, а MulMat4InPlace
// выше — нет: без умножения иерархия считалась бы неверно.
static size_t TransposeBlocks(const float* const[16], float*, size_t)
{
    static bool logged = false;
    if (!logged) {
        SDL_Log("TransformDataModule: no SIMD transpose on this platform - scalar path");
        logged = true;
    }
    return 0;
}
#endif

static void TransposeSoAToMatrices(const float* const src[16], float* dst, size_t n)
{
    TransposeScalar(src, dst, TransposeBlocks(src, dst, n), n);
}

void TransformDataModule::StoreTransforms(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene)
{
    UpdateLocalTransforms(om, scene);

    om->ForEachArchetype<Positions, DrawComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr, ComponentArray<DrawComponent, void>*)
    {
        const Positions& P = posArr->data;
        const size_t n = P.size();
        if (n == 0) return;

        float* dst = static_cast<float*>(
            bm->AcquireTransferWritePtr(task, safe_u32(n * 16 * sizeof(float))));
        if (!dst) return;

        const float* src[16];
        GatherPositionStreams(P, src);
        TransposeSoAToMatrices(src, dst, n);
    });
}

uint32_t TransformDataModule::AskNumTransform(ObjectManager* om, SceneData* scene)
{
    uint32_t num_transform = 0;

    om->ForEachArchetype<Positions, DrawComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr, ComponentArray<DrawComponent, void>*)
    {
        num_transform += safe_u32(posArr->size());
    });

    return num_transform;
}
