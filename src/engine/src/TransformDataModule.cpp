#include "PCH.h"
#include "TransformDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include <glm/gtc/type_ptr.hpp>   // glm::make_mat4
#include <unordered_set>
#include <immintrin.h>            // AVX: SoA→AoS транспонирование в StoreTransforms

TransformDataModule::TransformDataModule()
{
}

// Positions[i] ↔ glm::mat4. Раскладка движка (см. StoreTransforms): GPU-матрица
// column-major, столбцы = (x,y,z),(a,b,c),(e,f,g),(w,d,h), нижняя строка (i,j,k,l).
// glm тоже column-major, поэтому отображение прямое (без транспонирования).
static glm::mat4 LoadPositionMatrix(const Positions& P, size_t i)
{
    return glm::mat4(
        glm::vec4(P.x[i], P.y[i], P.z[i], P.i[i]),
        glm::vec4(P.a[i], P.b[i], P.c[i], P.j[i]),
        glm::vec4(P.e[i], P.f[i], P.g[i], P.k[i]),
        glm::vec4(P.w[i], P.d[i], P.h[i], P.l[i]));
}

static void StorePositionMatrix(Positions& P, size_t i, const glm::mat4& m)
{
    P.x[i] = m[0][0]; P.y[i] = m[0][1]; P.z[i] = m[0][2]; P.i[i] = m[0][3];
    P.a[i] = m[1][0]; P.b[i] = m[1][1]; P.c[i] = m[1][2]; P.j[i] = m[1][3];
    P.e[i] = m[2][0]; P.f[i] = m[2][1]; P.g[i] = m[2][2]; P.k[i] = m[2][3];
    P.w[i] = m[3][0]; P.d[i] = m[3][1]; P.h[i] = m[3][2]; P.l[i] = m[3][3];
}

// LocalMatrices[i] → glm::mat4. Раскладка column-major m0..m15 (как ждал make_mat4).
static glm::mat4 LoadLocalMatrix(const LocalMatrices& L, size_t i)
{
    return glm::mat4(
        glm::vec4(L.m0[i],  L.m1[i],  L.m2[i],  L.m3[i]),
        glm::vec4(L.m4[i],  L.m5[i],  L.m6[i],  L.m7[i]),
        glm::vec4(L.m8[i],  L.m9[i],  L.m10[i], L.m11[i]),
        glm::vec4(L.m12[i], L.m13[i], L.m14[i], L.m15[i]));
}

// lhs = lhs * rhs, in-place, БЕЗ glm-математики. 16 float'ов column-major на
// матрицу (m[col*4 + row]); SSE, столбец = один __m128.
//
// Каждый столбец результата — линейная комбинация ВСЕХ столбцов lhs, поэтому все
// 4 столбца lhs снимаются в c0..c3 ДО первой записи: иначе store столбца 0 затрёт
// данные, нужные столбцам 1..3 (in-place алиасинг). Эти 4 __m128 — не «лишние
// локали», а обязательные и бесплатные: живут в xmm-регистрах (4 из 16), в память
// не сбрасываются. Цикл по j с константной границей разворачивается компилятором,
// j и b как адресная арифметика исчезают.
static inline void MulMat4InPlace(float* lhs, const float* rhs)
{
    const __m128 c0 = _mm_loadu_ps(lhs + 0);
    const __m128 c1 = _mm_loadu_ps(lhs + 4);
    const __m128 c2 = _mm_loadu_ps(lhs + 8);
    const __m128 c3 = _mm_loadu_ps(lhs + 12);

    for (int j = 0; j < 4; ++j) {
        const float* b = rhs + j * 4;
        __m128 col =                 _mm_mul_ps(c0, _mm_set1_ps(b[0]));
        col = _mm_add_ps(col,        _mm_mul_ps(c1, _mm_set1_ps(b[1])));
        col = _mm_add_ps(col,        _mm_mul_ps(c2, _mm_set1_ps(b[2])));
        col = _mm_add_ps(col,        _mm_mul_ps(c3, _mm_set1_ps(b[3])));
        _mm_storeu_ps(lhs + j * 4, col);
    }
}

// Иерархия трансформов: для энтити с родителем и локальной матрицей пишем в его
// Positions полную композицию world = parent_world × local. Используется отладочными
// рамками коллайдеров (статичная локальная матрица — рамка сама следует за владельцем).
//
// Горячий путь (каждый кадр) — проход по КЭШУ разрезолвленных связей: без единого
// хэш-поиска. Сам резолв parent→(Positions*,index) (3 lookup'а в unordered_map на
// сущность) выполняется ТОЛЬКО при структурном изменении ECS: между изменениями адреса
// Archetype (std::map) и ComponentArray (unique_ptr) стабильны, а индексы/раскладка
// векторов меняются лишь на add/delete/load — все они взводят dirty_entity. Коммитим
// флаг здесь: единственный его потребитель — этот модуль (CalculateTransformSize читает
// его как size_fn РАНЬШЕ в кадре, до updater'а StoreTransforms), порядок соблюдён.
void TransformDataModule::UpdateLocalTransforms(ObjectManager* om, SceneData* scene)
{
    if (om->CheckNewObjects()) {
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
        om->NewObjectsCommit();
    }

    for (const LocalXformLink& r : local_links_) {
        // world — локальная копия матрицы родителя (левый операнд, аккумулятор).
        // Умножаем на локальную матрицу ребёнка ПРЯМО в world, без возврата новой
        // матрицы и без промежуточного make_mat4. Родитель в ECS не трогается —
        // мутируется только стековая копия.
        // world/local — здесь лишь контейнеры на 16 float'ов (column-major); в самом
        // умножении glm-математики нет — MulMat4InPlace работает по сырым указателям.
        // world/local — здесь лишь контейнеры на 16 float'ов (column-major); в самом
        // умножении glm-математики нет — MulMat4InPlace работает по сырым указателям.
        glm::mat4 world = LoadPositionMatrix(*r.parent_pos, r.parent_i);
        const glm::mat4 local = LoadLocalMatrix(*r.local, r.local_i);
        MulMat4InPlace(glm::value_ptr(world), glm::value_ptr(local));
        StorePositionMatrix(*r.child_pos, r.child_i, world);
    }
}

uint32_t TransformDataModule::CalculateTransformSize(ObjectManager* om, SceneData* scene)
{
    if (!om->CheckNewObjects()) {
        return total_size;
    }
    total_size = 0;

    om->ForEachArchetype<Positions, DrawComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<DrawComponent, void>*)
    {
        total_size += safe_u32(posArr->size()) * sizeof(PositionProxy16);
    }
    );

    return total_size;
}

// 16 SoA-потоков в порядке float'ов GPU-матрицы: столбцы = (x,y,z,i),(a,b,c,j),
// (e,f,g,k),(w,d,h,l) — та же раскладка, что в LoadPositionMatrix выше.
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

// Классическое транспонирование 8x8 float на AVX: после вызова r[j] содержит
// j-е элементы всех восьми входных регистров.
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

// SoA(16 потоков) → AoS(n матриц по 16 float). Блок из 8 энтити — это две матрицы
// 8x8 (потоки 0..7 и 8..15): после транспонирования lo[m]/hi[m] — первая/вторая
// половина m-й матрицы блока. Loadu/storeu — std::vector не даёт 32-байтового
// выравнивания. Интринсики компилируются без /arch:AVX, но исполняются только
// после рантайм-проверки SDL_HasAVX; хвост и fallback — скалярные.
static void TransposeSoAToMatrices(const float* const src[16], float* dst, size_t n)
{
    static const bool has_avx = SDL_HasAVX();

    size_t e = 0;
    if (has_avx) {
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
    }
    TransposeScalar(src, dst, e, n);
}

void TransformDataModule::StoreTransforms(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
	this->UpdateLocalTransforms(om, scene);

    // Порядок обхода совпадает с ForEach<DrawComponent, Positions> (одна и та же map
    // архетипов + индексы 0..n-1), так что порядок матриц в буфере не меняется.
    // SIMD-транспонирование целого архетипа НАПРЯМУЮ в mapped transfer-буфер — один
    // проход по памяти (чтение SoA + запись в tb), как при укладке колонок подряд;
    // ни промежуточного staging, ни memcpy по 64 байта на энтити.
    om->ForEachArchetype<Positions, DrawComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<DrawComponent, void>*)
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

    om->ForEachArchetype<Positions, DrawComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<DrawComponent, void>*)
    {
        num_transform += safe_u32(posArr->size());
    }
    );

    return num_transform;
}

void TransformDataModule::ReadBackCullingCountReader(BufferManager* bm, ReadBackTask* task)
{
    size_ptr = bm->ReadFromTransferBuffer(task, sizeof(uint32_t));
    std::cout << *reinterpret_cast<const uint32_t*>(size_ptr.data()) << "\n";
}

uint32_t TransformDataModule::CalculateOutTransformSize()
{
    if (size_ptr.size() < sizeof(uint32_t))
    {
        SDL_Log("Not enough data in size_ptr for uint32_t");
        return 0;
    }

    return *reinterpret_cast<const uint32_t*>(size_ptr.data()) * sizeof(glm::mat4);
}


//void TransformDataModule::StoreTransforms(BufferManager* bufferManager, ObjectManager* objectManager, SceneData* scene)
//{
//    UpdateLocalTransforms(objectManager, scene);
//
//    auto t0 = std::chrono::high_resolution_clock::now();
//    bufferManager->BeginWrite(buffer_data);
//
//    objectManager->ForEach<Positions>(scene, [&](const Positions& pos) {
//        Uint32 arr_size = static_cast<Uint32>(pos.size() * sizeof(float));
//
//        const f_restrict_pointer soa_ptrs[16] = {
//            pos.x.data(), pos.y.data(), pos.z.data(), pos.w.data(),
//            pos.a.data(), pos.b.data(), pos.c.data(), pos.d.data(),
//            pos.e.data(), pos.f.data(), pos.g.data(), pos.h.data(),
//            pos.i.data(), pos.j.data(), pos.k.data(), pos.l.data()
//        };
//        for (size_t i = 0; i < 16; ++i) {
//            bufferManager->AppendWrite(soa_ptrs[i], arr_size);
//            //bm->AppendWrite(ptr, arr_size);
//            //bm->uploadToGPUBuffer(buffer_data, soa_ptrs[i], arr_size);
//        }
//
//		//std::cout << "Stored " << pos.size() << " transforms.\n";
//    });
//
//    bufferManager->EndWrite();
//    auto t1 = std::chrono::high_resolution_clock::now();
//    double seconds = std::chrono::duration<double>(t1 - t0).count();
//    std::cout << "StoreTransforms taken " << seconds << " s" << "\n";
//}

//void TransformDataModule::StoreTransforms(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene)
//{
//    UpdateLocalTransforms(objectManager, scene);
//
//    chunks.clear();
//
//    std::unordered_set<const Positions*> seen;
//    seen.reserve(256);
//
//    objectManager->ForEach<Positions, ModelComponent>(scene,
//        [&](const Positions& pos, const ModelComponent& /*mod*/)
//    {
//        // Если это SoA-контейнер чанка: size() должно отражать кол-во элементов в чанке
//        if (pos.size() == 0) return;
//
//        const Positions* p = &pos;
//        if (seen.insert(p).second) { // добавили впервые
//            chunks.push_back(p);
//        }
//    });
//
//    if (chunks.empty()) return;
//
//    auto write_column = [&](auto Positions::* memberPtr) {
//        for (const Positions* p : chunks) {
//            const auto& vec = p->*memberPtr;
//            const Uint32 bytes = static_cast<Uint32>(vec.size() * sizeof(float));
//            bufferManager->UploadToTransferBuffer(task, bytes, vec.data());
//        }
//    };
//
//    write_column(&Positions::x);
//    write_column(&Positions::y);
//    write_column(&Positions::z);
//    write_column(&Positions::w);
//
//    write_column(&Positions::a);
//    write_column(&Positions::b);
//    write_column(&Positions::c);
//    write_column(&Positions::d);
//
//    write_column(&Positions::e);
//    write_column(&Positions::f);
//    write_column(&Positions::g);
//    write_column(&Positions::h);
//
//    write_column(&Positions::i);
//    write_column(&Positions::j);
//    write_column(&Positions::k);
//    write_column(&Positions::l);
//}

