// GPU-каллинг С КОМПАКТАЦИЕЙ (+ опциональный отсев по экранному размеру, см. CullParams). Одна ПРОГРАММА НА ПРОХОД с батчами: каждая биндит камерный
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
StructuredBuffer<uint>    EntityToCmd  : register(t1, space0);   // запись -> команда k + диапазон сабмеша
StructuredBuffer<float4>  BoundSpheres : register(t2, space0);   // по строкам: xyz центр (model), w радиус; w<0 — нет модели
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Cameras   : register(t3, space0);   // буфер группы камер (биндится программой)
StructuredBuffer<float4x4> Transforms  : register(t4, space0);

RWStructuredBuffer<int> OutPib   : register(u0, space1);   // компактный выход
RWByteAddressBuffer     Indirect : register(u1, space1);   // команды регионов: атомик num_instances + чтение first_instance

cbuffer CullParams : register(b0, space2) {
    uint  range_start;         // первая PIB-запись, которую обрабатывает эта программа
    uint  range_count;         // сколько записей (= размер диспатча)
    uint  num_blocks;          // блоков региона; для блока b тестируется Cameras[b]
    uint  cmd_base;            // база региона прохода в индиректе, в командах
    uint  commands;            // команд на блок = страйд внутри региона
    float min_screen_radius_px; // отсев по экранному размеру; 0 = выключен
    uint  target_height;        // высота цветового таргета прохода, px
    uint  invert_span;          // 1 = рисуем то, что НИЖЕ нижней границы (сплат-проход)
};

static const uint CMD_STRIDE = 20u;   // sizeof(SDL_GPUIndexedIndirectDrawCommand); num_instances@4, first_instance@16

// Слово EntityToCmd: младшие 24 бита — индекс команды, старшие два ниббла — ступени диапазона
// экранных размеров, на котором сабмеш записи рисуется. Раскладку задаёт PackLodRange в
// ModelData.h (там же смысл поля), здесь только разбор.
static const uint CMD_INDEX_MASK = 0x00FFFFFFu;

// Ступень 0 = граница не задана, тогда отдаём unset (0 для нижней, +бесконечность для верхней) —
// сабмеш без проставленного диапазона ведёт себя ровно как до появления поля.
float LodBoundPx(uint level, float unset)
{
    return (level == 0u) ? unset : 0.5 * exp2(float(level - 1u));
}

// Радиус ограничивающей сферы в ПИКСЕЛЯХ приёмника. w клипа = расстояние вдоль оси взгляда
// (для mul(M,v) это ровно dot(vp[3], p) — полный mul не нужен, берём одну строку), proj[1][1] —
// вертикальный фокус, а NDC по вертикали занимает 2 единицы на target_height пикселей.
// Аппроксимация radius/w вместо radius/sqrt(w^2-r^2) точна ровно там, где нас это волнует:
// у мелочи w >> r. Вызывать только для записей, ПРОШЕДШИХ фрустум — иначе w может быть <= 0.
float ScreenRadiusPx(float4x4 vp, float4x4 proj, float3 center, float radius)
{
    float w = dot(vp[3], float4(center, 1.0));
    return radius * proj[1][1] * (0.5 * float(target_height)) / max(w, 1e-4);
}

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

    uint word = EntityToCmd[i];
    uint k    = word & CMD_INDEX_MASK;   // индекс команды ЛОКАЛЬНЫЙ для прохода (см. StoreEntityToCmd)
    // Границы от камеры не зависят — разбираем до цикла по блокам.
    float span_min = LodBoundPx((word >> 24) & 0xFu, 0.0);
    float span_max = LodBoundPx((word >> 28) & 0xFu, 1e30);

    // Обычный проход рисует объект В диапазоне сабмеша, сплат-проход — НИЖЕ его нижней границы.
    // Оба теста — из ОДНОЙ пары ступеней: порог живёт в одном месте, а какую его сторону взять,
    // решает проход. Два числа, обязанных совпадать, не заводятся, значит и разъехаться нечему.
    // Нижняя граница не задана (ступень 0) → у сплата выходит [0, 0), то есть пусто: пока порог
    // не выставлен, сплат-проход не рисует ничего сам собой.
    float lod_min = (invert_span != 0u) ? 0.0      : span_min;
    float lod_max = (invert_span != 0u) ? span_min : span_max;

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
        if (!has_geom) { ScatterInto(b, k, row); continue; }   // transformless — видим всегда

        float4x4 vp = mul(Cameras[b].proj, Cameras[b].view);
        if (!SphereVisible(vp, center, radius)) continue;

        float px = ScreenRadiusPx(vp, Cameras[b].proj, center, radius);

        // Отсев мелочи: глобальный порог прохода и собственная нижняя граница сабмеша — про разное
        // (пыль против неразрешимой детали), поэтому они не заменяют друг друга, а складываются по
        // максимуму. Оба выключены → условие ложно всегда (радиус >= 0), поведения не меняют.
        // Для сплат-прохода глобальный порог означает АБСОЛЮТНЫЙ пол, ниже которого не рисуется
        // вообще ничего: он обязан стоять ниже L, иначе срежет и сплаты вместе с оригиналами.
        if (px < max(min_screen_radius_px, lod_min)) continue;
        // Верхняя граница — место, которое сабмеш уступает более грубому уровню: уровни модели идут
        // соседними сабмешами с непересекающимися диапазонами, поэтому видимым остаётся ровно один.
        if (px >= lod_max) continue;

        ScatterInto(b, k, row);
    }
}
