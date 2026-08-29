#ifndef MAIN_MATERIAL_API_HLSL
#define MAIN_MATERIAL_API_HLSL

// Движковый ПРОЛОГ material-API для main-пасса. Прячет всё движко-определённое, чтобы
// пользовательский surface содержал только `cbuffer MaterialBlock` + `getSurface`.
// Регистры материальных текстур — t2..t5 (слоты 0/1 заняты глобалками пасса: тень + env-кубмапа):
// albedo t2, normal t3, orm t4, emissive t5.

#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"

// ── Потолки раскладки вариантов ──
// Значения приходят ДЕФАЙНАМИ (Engine::InitDefaultShaders, они же в ключе кэша .spv). Дефолты
// ниже держат шейдеры, собранные БЕЗ них (напр. пользовательские surface из кода игры) — значения
// обязаны совпадать с C++ (ShaderTypes.h). Объявлены В НАЧАЛЕ пролога: ниже они уже нужны
// размером textures[].
#ifndef MAX_SLOTS
#define MAX_SLOTS 12
#endif
#ifndef MAX_VARIATIVE_SLOTS
#define MAX_VARIATIVE_SLOTS 4
#endif
#ifndef MAX_UVL_BLOCKS
#define MAX_UVL_BLOCKS 32
#endif

// Контракт VS→PS (вход getSurface) и поверхности (выход). Движковый: PSInput = выход
// вершинника, SurfaceData потребляет main базы. Идентичен в обоих пассах (тот же
// main_pass.vert) → дублируется в двух прологах; правка контракта = оба пролога + VSOutput вершинника.
struct PSInput
{
    float4 sv_pos                               : SV_Position;
    [[vk::location(0)]] float2 v_uv             : TEXCOORD0;
    [[vk::location(1)]] float3 v_worldPos       : TEXCOORD1;
    [[vk::location(2)]] float3 v_worldNormal    : TEXCOORD2;
    [[vk::location(3)]] float3 v_worldTangent   : TEXCOORD3;
    [[vk::location(4)]] float3 v_worldBitangent : TEXCOORD4;
    [[vk::location(5)]] float  v_alpha          : TEXCOORD5;
    // Строка трансформа инстанса (-1 = отсечён). Член контракта VS→PS: вершинник общий на три
    // пролога, разъехавшийся PSInput = молча битые локейшены.
    [[vk::location(6)]] nointerpolation int v_row : TEXCOORD6;
};

struct SurfaceData
{
    float3 baseColor;
    float3 normal;     // world space
    float  alpha;
    float3 emission;   // добавляется к освещению в main базы
    float  metallic;   // сила спекуляра (Blinn-Phong)
    float  roughness;  // → shininess
    float  ao;         // ambient occlusion (модулирует ambient/env в базе); 1 = нет затенения
};

// Слоты 0/1 заняты глобалками пасса (тень t0/s0, env-кубмапа t1/s1 — объявлены базой),
// материальные текстуры идут после: albedo t2, normal t3, orm t4, emissive t5.
[[vk::combinedImageSampler]]
Texture2DArray u_albedo        : register(t2, space2);
[[vk::combinedImageSampler]]
SamplerState   u_albedoSampler : register(s2, space2);

[[vk::combinedImageSampler]]
Texture2DArray u_normal        : register(t3, space2);
[[vk::combinedImageSampler]]
SamplerState   u_normalSampler : register(s3, space2);

// ORM (упаковка): R=AO, G=Roughness, B=Metallic. Одна текстура → один сэмпл, один UVL.
[[vk::combinedImageSampler]]
Texture2DArray u_orm           : register(t4, space2);
[[vk::combinedImageSampler]]
SamplerState   u_ormSampler    : register(s4, space2);

[[vk::combinedImageSampler]]
Texture2DArray u_emissive      : register(t5, space2);
[[vk::combinedImageSampler]]
SamplerState   u_emissiveSampler : register(s5, space2);

struct TextureData { uint4 data; };
cbuffer TextureUVLBlock : register(b0, space3) {
    // Таблица СГРУППИРОВАНА ПО СЛОТАМ: подряд все блоки слота, внутри группы [0] — дефолт.
    // Поэтому индекс блока слота s НЕ равен s — только через TexIndex(s) (см. ниже).
    // У материала без вариантов таблица вырождается в прежнюю плотную по required_slots.
    TextureData textures[MAX_UVL_BLOCKS];
};

#define MATERIAL_BLOCK_REGISTER register(b1, space3)

// Регистры движковых storage-буферов базы зависят от числа сэмплеров пасса (shadercross
// нумерует storage ПОСЛЕ сэмплеров). Здесь 6 сэмплеров (shadow t0 + env t1 + albedo t2 +
// normal t3 + orm t4 + emissive t5) → LightBlock t6, ShadowCameras t7, Camera t8.
// Height НЕ добавляет сэмплер: живёт в альфе u_normal → регистры не сдвигаются.
#define LIGHT_BLOCK_REGISTER     register(t6, space2)
#define SHADOW_CAMERAS_REGISTER  register(t7, space2)
#define CAMERA_REGISTER          register(t8, space2)
// ── Переключаемые варианты текстур ──

// Как адресовать textures[]: слово на слот + номер материала у сущности. Третий uniform, ПОСЛЕ
// MaterialBlock — чтобы не двигать его регистр. Зеркало VariantLayout из RenderCommandData.h.
cbuffer VariantLayoutBlock : register(b2, space3) {
    uint4 slot_layout[MAX_SLOTS / 4];   // (base<<16)|(cell<<8)|count на слот
    uint  material_index;               // offset 48 — массив кончается на 16-байтной границе
};

// ПЕРЕКЛЮЧЕНИЕ вариантов — OPT-IN по дефайну TEXTURE_VARIANTS (его ставит Engine своим fs).
// Пролог общий: его включают и пользовательские surface из кода игры, а два буфера ниже
// обязана биндить КАЖДАЯ sp, чей fs их объявил, — иначе «Missing fragment storage buffer
// binding». Без дефайна шейдер работает как раньше и показывает ДЕФОЛТ слота: адресация через
// base остаётся (она в пуше, буферов не требует), пропадает только выбор варианта.
#ifdef TEXTURE_VARIANTS
// Префикс (int на строку трансформа) и сами номера вариантов. Оба ФРАГМЕНТНЫЕ: вершинник общий
// с чужими sp, и буфер в ЕГО списке пришлось бы биндить всем им без разбора.
// 6 сэмплеров + LightBlock t6 + ShadowCameras t7 + Camera t8 → префикс t9, состояния t10.
StructuredBuffer<int>  TexStatePrefix : register(t9, space2);
StructuredBuffer<uint> TexState       : register(t10, space2);

// Инициализируются базой ОДИН раз за пиксель (BEGIN_MATERIAL_API), чтобы не менять сигнатуры
// SampleAlbedo(uv)/getSurface и не ломать пользовательские surface.hlsl.
static int  g_stateOfs   = -1;
static uint g_sectionOfs = 0;
#define BEGIN_MATERIAL_API(input) { g_stateOfs = ((input).v_row < 0) ? -1 : TexStatePrefix[(input).v_row]; \
                                    g_sectionOfs = material_index * MAX_VARIATIVE_SLOTS; }
#else
#define BEGIN_MATERIAL_API(input)
#endif

// Индекс блока UVL для слота s. s — литерал на всех вызовах, обе индексации статические.
// Три проверки, и роли у них РАЗНЫЕ:
//   g_stateOfs >= 0  — БЕЗОПАСНОСТЬ: элемента у сущности нет, индекс ушёл бы в минус;
//   v >= count       — КОРРЕКТНОСТЬ: номер протух (вариант удалили, материал сменили);
//   count > 1        — СТОИМОСТЬ: у невариативного слота её случай уже закрыт клампом (cell==0,
//                      прочиталась бы чужая ячейка, а кламп сшиб бы её в 0), но без неё
//                      storage-буфер читал бы КАЖДЫЙ пиксель КАЖДОГО материала, включая все
//                      невариативные. Заодно объявление намерения.
uint TexIndex(uint s)
{
    uint L     = slot_layout[s >> 2][s & 3];
    uint count = L & 0xFFu;
    uint v = 0u;
#ifdef TEXTURE_VARIANTS
    if (count > 1u && g_stateOfs >= 0)
        v = TexState[g_stateOfs + g_sectionOfs + ((L >> 8) & 0xFFu)];
    if (v >= count) v = 0u;
#endif
    return (L >> 16) + v;   // ИНДЕКС БЛОКА СЛОТА НЕ РАВЕН s — только через base
}


// Камера объявлена ЗДЕСЬ (не в базе), чтобы getSurface мог посчитать view-вектор для POM
// ДО включения базы. База (main_pass.frag.hlsl) её только использует. Дублируется в текстурелесс
// прологе (правка = оба пролога). Раскладка совпадает с CameraData вершинника/базы.
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : CAMERA_REGISTER;

float4 SampleAlbedo(float2 uv)
{
    return sampleAtlas(u_albedo, u_albedoSampler, textures[TexIndex(0)].data, uv);
}
float3 SampleNormalWorld(PSInput input, float2 uv, bool isFrontFace)
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[TexIndex(1)].data,
        uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    return isFrontFace ? n : -n;
}

// POM: смещённый mesh-UV. view-вектор строим из камеры (camPos = -R^T·t из view, как в базе)
// и переводим в tangent space через TBN. heightScale — глубина рельефа из MaterialBlock (0 = off);
// pomBias — АБСОЛЮТНЫЙ пол LOD рельефа (mip L = среднее по 2^L×2^L соседям → глушит шум карты на
// любой дистанции; 0 = полная детализация). Глубина — из альфы normal-текстуры (textures[1]).
float2 ApplyParallax(PSInput input, float heightScale, float pomBias)
{
    // Градиенты mesh-UV считаем ДО любых ветвлений/циклов (деривативы легальны только в uniform
    // control flow). heightScale — uniform, поэтому ранний выход ниже когерентен по варпу.
    float2 ddx_uv = ddx(input.v_uv);
    float2 ddy_uv = ddy(input.v_uv);
    if (heightScale <= 0.0) return input.v_uv;

    // LOD рельефа: один раз до марча (внутри цикла — SampleLevel). Базовый экранный LOD считаем
    // руками из градиентов в ТЕКСЕЛЯХ атласа (та же формула, что у аппаратного LOD), затем
    // поднимаем до пола pomBias. max() оставляет анти-алиасинг вдали (там базовый LOD больше).
    uint tw, th, tlayers;
    u_normal.GetDimensions(tw, th, tlayers);
    float2 nsc     = unpackUnorm2x16(textures[TexIndex(1)].data.y);
    float2 gx      = ddx_uv * nsc * float2(tw, th);
    float2 gy      = ddy_uv * nsc * float2(tw, th);
    float  lodBase = 0.5 * log2(max(dot(gx, gx), dot(gy, gy)));
    float  lod     = max(lodBase, pomBias);

    float3   camPos = -mul(transpose((float3x3)Camera[0].view), Camera[0].view._m03_m13_m23);
    float3   Vw     = normalize(camPos - input.v_worldPos);
    float3x3 TBN    = float3x3(
        normalize(input.v_worldTangent),
        normalize(input.v_worldBitangent),
        normalize(input.v_worldNormal));
    float3   Vt     = normalize(mul(TBN, Vw));   // world → tangent (строки TBN = базис)
    // Изнанка поверхности (пассы рисуют без куллинга): камера «под» поверхностью, марч вниз
    // геометрически бессмыслен и даёт мусорные пересечения → без смещения.
    if (Vt.z <= 0.0) return input.v_uv;

    // mirrorTile: у кромки марч уходит за тайл (до ~heightScale) — там продолжаем рельеф и
    // контент ЗЕРКАЛОМ (см. mirrorTile в material.hlsl). Внутри [0..1] это ТОЧНАЯ identity —
    // интерьер не меняется ни на йоту; кромка получает непрерывное поле вместо «стены»
    // (clamp прилипал к крайним текселям, wrap бился о несшитый край) → растянутой каймы нет.
    return mirrorTile(parallaxOcclusionUV(u_normal, u_normalSampler, textures[TexIndex(1)].data,
                               input.v_uv, Vt, heightScale, lod));
}
// ORM: возвращает сырой RGB (R=AO, G=Roughness, B=Metallic) — разбирает getSurface.
float3 SampleORM(float2 uv)
{
    return sampleAtlas(u_orm, u_ormSampler, textures[TexIndex(2)].data, uv).rgb;
}
float3 SampleEmissive(float2 uv)
{
    return sampleAtlas(u_emissive, u_emissiveSampler, textures[TexIndex(3)].data, uv).rgb;
}

#endif
