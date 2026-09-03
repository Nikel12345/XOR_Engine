#ifndef MAIN_UNTEXTURED_MATERIAL_API_HLSL
#define MAIN_UNTEXTURED_MATERIAL_API_HLSL

// Пролог material-API для ТЕКСТУРЕЛЕСС main-материала. Текстур/сэмплеров нет — цвет из
// фактора, нормаль геометрическая. Контракт PSInput/SurfaceData тот же, что у текстурного
// пролога (продублирован). Слоты сдвинуты: единственный сэмплер пасса — тень (t0, объявляет
// база), поэтому storage базы съезжают на t1/t2, а MaterialBlock (первый uniform) — на b0.

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
    // пролога, разъехавшийся PSInput = молча битые локейшены. Здесь не используется.
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
    float  ao;         // контракт с базой (нет AO-карты → всегда 1)
};

// Число источников света в LIGHT_BUFFER — ПЕРВЫЙ fragment-uniform (b0): его пушит push-функция
// программы (RP::LightCountPushData), а UVL/params/раскладка встают за ним — движок кладёт их с
// binder.frag_count. Счётчик приходит push-константой, а НЕ из LightBlock.GetDimensions: буфер
// умеет только расти, и его размер больше числа источников (в пределе — свет прошлой сцены).
cbuffer LightCountBlock : register(b0, space3) {
    uint u_lightCount;
};

// UVL нет → MaterialBlock идёт сразу за счётчиком светов (b1). Сэмплеров пасса 2 (тень t0 +
// env t1, оба объявлены базой), поэтому storage базы съезжают: LightBlock t2, ShadowCameras t3.
#define MATERIAL_BLOCK_REGISTER  register(b1, space3)
#define LIGHT_BLOCK_REGISTER     register(t2, space2)
#define SHADOW_CAMERAS_REGISTER  register(t3, space2)
#define CAMERA_REGISTER          register(t4, space2)   // 2 сэмплера (тень+env) + LightBlock(t2) + ShadowCameras(t3) → Camera t4

// Таблицы UVL здесь нет, значит нет и вариантов: пролог держит только ПУСТУЮ форму общего
// контракта с базой (та зовёт BEGIN_MATERIAL_API вне зависимости от пролога).
#define BEGIN_MATERIAL_API(input)

// Камера объявлена в прологе (симметрично текстурному: там getSurface считает POM-view до базы).
// Текстурелесс POM не использует, но объявление держим здесь, чтобы база была единой (без Camera).
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : CAMERA_REGISTER;

#endif
