// GPU-каллинг С КОМПАКТАЦИЕЙ. Одна ПРОГРАММА НА ПРОХОД с батчами: каждая биндит камерный
// буфер своего прохода (Cameras) и обрабатывает его диапазон PIB-записей [range_start,
// +range_count), раскидывая выживших по блокам его РЕГИОНА индиректа.
//
// Регион прохода в индиректе: num_blocks блоков по commands команд, начиная с команды cmd_base.
// Блок = один дроу прохода за кадр; обычно это камера (для блока b тестируется Cameras[b]),
// но проход может не отсекаться вовсе — тогда сферы его записей приходят с w<0 и Cameras
// не читается. Команда k (индекс ЛОКАЛЬНЫЙ для прохода, так его пишет EntityToCmd) блока b
// лежит в слоте cmd_base + b*commands + k.
//
// Адрес в out_pib НЕ считается здесь: его несёт сама команда. StoreIndirect кладёт в first_instance
// АБСОЛЮТНЫЙ адрес куска записей этой команды у этого блока, поэтому и скаттер, и вершинник
// (SV_InstanceID = first_instance + i) адресуют out_pib без арифметики блоков.
// num_instances @4, first_instance @16 в команде.

StructuredBuffer<int>     PIB          : register(t0, space0);   // запись -> строка трансформа (-1 = transformless, всегда видим)
StructuredBuffer<uint>    EntityToCmd  : register(t1, space0);   // запись -> индекс команды k В СВОЕЙ ГРУППЕ
StructuredBuffer<float4>  BoundSpheres : register(t2, space0);   // по строкам: xyz центр (model), w радиус; w<0 — нет модели
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Cameras   : register(t3, space0);   // буфер группы камер (биндится программой)
StructuredBuffer<float4x4> Transforms  : register(t4, space0);

RWStructuredBuffer<int> OutPib   : register(u0, space1);   // компактный выход
RWByteAddressBuffer     Indirect : register(u1, space1);   // команды регионов: атомик num_instances + чтение first_instance

cbuffer CullParams : register(b0, space2) {
    uint range_start;      // первая PIB-запись, которую обрабатывает эта программа
    uint range_count;      // сколько записей (= размер диспатча)
    uint num_blocks;       // блоков региона; для блока b тестируется Cameras[b]
    uint cmd_base;         // база региона прохода в индиректе, в командах
    uint commands;         // команд на блок = страйд внутри региона
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

// Кладёт выжившую запись в блок b, команду k своего прохода.
void ScatterInto(uint b, uint k, int row)
{
    uint cmd = cmd_base + b * commands + k;
    uint slot;
    Indirect.InterlockedAdd(cmd * CMD_STRIDE + 4u, 1u, slot); // +4 = num_instances
    // +16 = first_instance: абсолютный адрес куска этой команды в out_pib (пишет StoreIndirect).
    uint first_instance = Indirect.Load(cmd * CMD_STRIDE + 16u);
    OutPib[first_instance + slot] = row;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint local = tid.x;
    if (local >= range_count) return;
    uint i = range_start + local;               // запись в диапазоне СВОЕГО прохода

    int row = PIB[i];   // -1 = transformless (нет Positions): строки/сферы нет — видим всегда

    uint k = EntityToCmd[i];   // индекс команды ЛОКАЛЬНЫЙ для прохода (см. StoreEntityToCmd)

    // Мировые центр/радиус — один раз (не зависят от камеры). w<0 → нет геометрии, видим всегда.
    // row<0 идёт тем же путём: сфера-заглушка w=-1 → безусловный скаттер во все блоки прохода
    // (-1 уезжает в out_pib — читатели трактуют его как вырожденный, а transformless-VS позицию
    // строит сам и out_pib не читает). [branch] обязателен: flatten прочитал бы BoundSpheres[-1].
    float4 sphere = float4(0.0, 0.0, 0.0, -1.0);
    [branch] if (row >= 0) sphere = BoundSpheres[row];
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

    // Блоки региона: для b-го тестируем Cameras[b] (если у записи есть геометрия) и пишем в него.
    for (uint b = 0; b < num_blocks; ++b) {
        bool vis = !has_geom || SphereVisible(mul(Cameras[b].proj, Cameras[b].view), center, radius);
        if (vis) ScatterInto(b, k, row);
    }
}
