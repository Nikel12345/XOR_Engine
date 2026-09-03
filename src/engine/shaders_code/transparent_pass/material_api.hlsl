#ifndef TRANSPARENT_MATERIAL_API_HLSL
#define TRANSPARENT_MATERIAL_API_HLSL

// Движковый ПРОЛОГ material-API для прозрачного пасса. Регистры материальных текстур — t0/t1
// (теней нет, глобалок на слоте 0 нет → сэмплеры с t0). Контракт PSInput/SurfaceData тот же,
// что в main-прологе (продублирован ниже). User-surface = `cbuffer MaterialBlock` + `getSurface`.

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

// Контракт VS→PS (вход getSurface) и поверхности (выход) — движковый, идентичен main-пассу
// (тот же main_pass.vert). Дублируется здесь и в main_pass/material_api.hlsl.
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
};

[[vk::combinedImageSampler]]
Texture2DArray u_albedo        : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerState   u_albedoSampler : register(s0, space2);

[[vk::combinedImageSampler]]
Texture2DArray u_normal        : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState   u_normalSampler : register(s1, space2);

struct TextureData { uint4 data; };
// Число источников света в LIGHT_BUFFER — ПЕРВЫЙ fragment-uniform (b0): его пушит push-функция
// программы (RP::LightCountPushData), а UVL/params/раскладка встают за ним — движок кладёт их с
// binder.frag_count. Счётчик приходит push-константой, а НЕ из LightBlock.GetDimensions: буфер
// умеет только расти, и его размер больше числа источников (в пределе — свет прошлой сцены).
cbuffer LightCountBlock : register(b0, space3) {
    uint u_lightCount;
};

cbuffer TextureUVLBlock : register(b1, space3) {
    // Сгруппирована по слотам, индекс блока — через TexIndex(s). См. main-пролог.
    TextureData textures[MAX_UVL_BLOCKS];
};

#define MATERIAL_BLOCK_REGISTER register(b2, space3)
// ── Переключаемые варианты текстур ──

// Как адресовать textures[]: слово на слот + номер материала у сущности. Третий uniform, ПОСЛЕ
// MaterialBlock — чтобы не двигать его регистр. Зеркало VariantLayout из RenderCommandData.h.
cbuffer VariantLayoutBlock : register(b3, space3) {
    uint4 slot_layout[MAX_SLOTS / 4];   // (base<<16)|(cell<<8)|count на слот
    uint  material_index;               // offset 48 — массив кончается на 16-байтной границе
};

// ПЕРЕКЛЮЧЕНИЕ вариантов — OPT-IN по дефайну TEXTURE_VARIANTS (его ставит Engine своим fs).
// Пролог общий: его включают и пользовательские surface из кода игры, а два буфера ниже
// обязана биндить КАЖДАЯ sp, чей fs их объявил, — иначе «Missing fragment storage buffer
// binding». Без дефайна шейдер работает как раньше и показывает ДЕФОЛТ слота: адресация через
// base остаётся (она в пуше, буферов не требует), пропадает только выбор варианта.
#ifdef TEXTURE_VARIANTS
// Разреженный канал состояний: адресацию даёт SPARSE_CHANNEL (sparse_rank.hlsli), данные —
// TexStateIndex/TexState. Все три буфера ФРАГМЕНТНЫЕ: вершинник общий с чужими sp, и буфер в ЕГО
// списке пришлось бы биндить всем им без разбора.
// 2 сэмплера (albedo t0, normal t1) + LightBlock t2 (объявляет база) → rank t3, index t4,
// состояния t5.
#include "sparse_rank.hlsli"
SPARSE_CHANNEL(TexState, t3, space2)                            // → TexStateWords, TexStateRank(row)
StructuredBuffer<uint> TexStateIndex : register(t4, space2);    // смещение ячеек носителя
StructuredBuffer<uint> TexState      : register(t5, space2);

// -1 = переключать нечего: либо строки нет вовсе (transformless, PIB = -1), либо она не носитель.
// Дальше по коду важен только знак.
int TexStateOfs(int row)
{
    const int r = TexStateRank(row);
    return (r < 0) ? -1 : int(TexStateIndex[r]);
}

// Инициализируются базой ОДИН раз за пиксель (BEGIN_MATERIAL_API), чтобы не менять сигнатуры
// SampleAlbedo(uv)/getSurface и не ломать пользовательские surface.hlsl.
static int  g_stateOfs   = -1;
static uint g_sectionOfs = 0;
#define BEGIN_MATERIAL_API(input) { g_stateOfs = TexStateOfs((input).v_row); \
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


float4 SampleAlbedo(float2 uv)
{
    return sampleAtlas(u_albedo, u_albedoSampler, textures[TexIndex(0)].data, uv);
}
float3 SampleNormalWorld(PSInput input, bool isFrontFace)
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[TexIndex(1)].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    return isFrontFace ? n : -n;
}

#endif
