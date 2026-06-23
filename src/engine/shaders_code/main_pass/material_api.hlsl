#ifndef MAIN_MATERIAL_API_HLSL
#define MAIN_MATERIAL_API_HLSL

// ─────────────────────────────────────────────────────────────────────────────
// Движковый ПРОЛОГ material-API для main-пасса. Прячет всё движко-определённое, чтобы
// пользовательский surface содержал только `cbuffer MaterialBlock` + `getSurface`.
// Регистры материальных текстур — t1/t2 (слот 0/space2 занят глобалкой теней пасса).
// ─────────────────────────────────────────────────────────────────────────────

#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"

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
};

struct SurfaceData
{
    float3 baseColor;
    float3 normal;     // world space
    float  alpha;
    float3 emission;   // добавляется к освещению в main базы
    float  metallic;   // сила спекуляра (Blinn-Phong)
    float  roughness;  // → shininess
};

[[vk::combinedImageSampler]]
Texture2DArray u_albedo        : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState   u_albedoSampler : register(s1, space2);

[[vk::combinedImageSampler]]
Texture2DArray u_normal        : register(t2, space2);
[[vk::combinedImageSampler]]
SamplerState   u_normalSampler : register(s2, space2);

struct TextureData { uint4 data; };
cbuffer TextureUVLBlock : register(b0, space3) {
    TextureData textures[2];   // albedo + normal — плотный UVL по required_slots
};

#define MATERIAL_BLOCK_REGISTER register(b1, space3)

// Регистры движковых storage-буферов базы зависят от числа сэмплеров пасса (shadercross
// нумерует storage ПОСЛЕ сэмплеров). Здесь 3 сэмплера (shadow t0 + albedo t1 + normal t2) →
// LightBlock t3, ShadowCameras t4. База берёт их через эти макросы (включается после пролога).
#define LIGHT_BLOCK_REGISTER     register(t3, space2)
#define SHADOW_CAMERAS_REGISTER  register(t4, space2)
#define CAMERA_REGISTER          register(t5, space2)   // 3 сэмплера + LightBlock(t3) + ShadowCameras(t4) → Camera t5

float4 SampleAlbedo(float2 uv)
{
    return sampleAtlas(u_albedo, u_albedoSampler, textures[0].data, uv);
}
float3 SampleNormalWorld(PSInput input, bool isFrontFace)
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[1].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    return isFrontFace ? n : -n;
}

#endif
