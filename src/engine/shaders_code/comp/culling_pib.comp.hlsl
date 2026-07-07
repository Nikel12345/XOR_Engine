// GPU-каллинг С КОМПАКТАЦИЕЙ. Одна ПРОГРАММА на группу камер (игрок / свет / …): каждая биндит
// СВОЙ камерный буфер (Cameras) и обрабатывает СВОЙ диапазон PIB-записей [range_start, +range_count),
// раскидывая выживших в блоки [block_base, block_base + num_blocks). Никакого is_shadow — какие
// камеры / блоки / записи брать задаёт программа (push + биндинг камерного буфера). PIB сгруппирован
// по проходам, поэтому диапазон программы непрерывен: теневые записи [0, shadow_pib), main [shadow_pib, N).
//
// out_pib[c*block_stride + first_instance_k + slot] = row (компактно, НА МЕСТЕ внутри команды);
// indirect[c*total_commands + k].num_instances += 1 (атомик). c = block_base + b, Cameras[b] — b-я
// камера группы. block_stride = N (всего PIB-записей). first_instance/num_instances @16/@4 в команде.

StructuredBuffer<int>     PIB          : register(t0, space0);   // запись -> строка трансформа (-1 = протухшая)
StructuredBuffer<uint>    EntityToCmd  : register(t1, space0);   // запись -> глобальный индекс команды k
StructuredBuffer<float4>  BoundSpheres : register(t2, space0);   // по строкам: xyz центр (model), w радиус; w<0 — нет модели
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Cameras   : register(t3, space0);   // буфер группы камер (биндится программой)
StructuredBuffer<float4x4> Transforms  : register(t4, space0);

RWStructuredBuffer<int> OutPib   : register(u0, space1);   // компактный выход
RWByteAddressBuffer     Indirect : register(u1, space1);   // per-camera команды: атомик num_instances + чтение first_instance

cbuffer CullParams : register(b0, space2) {
    uint range_start;      // первая PIB-запись, которую обрабатывает эта программа
    uint range_count;      // сколько записей (= размер диспатча)
    uint block_base;       // первый камерный блок программы (0 игрок, 1 свет, …)
    uint num_blocks;       // число камер в группе (Cameras[0..num_blocks))
    uint block_stride;     // N — всего PIB-записей (страйд блока out_pib)
    uint total_commands;   // TC — страйд блока индиректа (команд на камеру)
};

static const uint CMD_STRIDE = 20u;   // sizeof(SDL_GPUIndexedIndirectDrawCommand); num_instances@4, first_instance@16

bool SphereVisible(float4x4 vp, float3 center, float radius)
{
    float4 planes[6] = {
        vp[3] + vp[0], vp[3] - vp[0],
        vp[3] + vp[1], vp[3] - vp[1],
        vp[3] + vp[2], vp[3] - vp[2],
    };
    [unroll]
    for (int p = 0; p < 6; ++p) {
        if (dot(planes[p].xyz, center) + planes[p].w < -radius * length(planes[p].xyz))
            return false;
    }
    return true;
}

// Кладёт выжившую запись в блок камеры c, команду k, начиная с first_instance_k.
void ScatterInto(uint c, uint k, uint first_instance_k, int row)
{
    uint slot;
    Indirect.InterlockedAdd((c * total_commands + k) * CMD_STRIDE + 4u, 1u, slot); // +4 = num_instances
    OutPib[c * block_stride + first_instance_k + slot] = row;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint local = tid.x;
    if (local >= range_count) return;
    uint i = range_start + local;               // запись в СВОЁМ диапазоне прохода

    int row = PIB[i];
    if (row < 0) return;                        // протухшая запись — не рисуем

    uint k = EntityToCmd[i];
    uint first_instance_k = Indirect.Load(k * CMD_STRIDE + 16u); // +16 = first_instance (одинаков во всех блоках)

    // Мировые центр/радиус — один раз (не зависят от камеры). w<0 → нет геометрии, видим всегда.
    float4 sphere = BoundSpheres[row];
    bool has_geom = (sphere.w >= 0.0);
    float3 center = float3(0, 0, 0);
    float  radius = 0.0;
    if (has_geom) {
        float4x4 m = Transforms[row];
        center = mul(m, float4(sphere.xyz, 1.0)).xyz;
        float3 sc = float3(
            length(float3(m[0][0], m[1][0], m[2][0])),
            length(float3(m[0][1], m[1][1], m[2][1])),
            length(float3(m[0][2], m[1][2], m[2][2])));
        radius = sphere.w * max(sc.x, max(sc.y, sc.z));
    }

    // Камеры группы: Cameras[b] тестируем, пишем в блок block_base + b.
    for (uint b = 0; b < num_blocks; ++b) {
        bool vis = !has_geom || SphereVisible(mul(Cameras[b].proj, Cameras[b].view), center, radius);
        if (vis) ScatterInto(block_base + b, k, first_instance_k, row);
    }
}
