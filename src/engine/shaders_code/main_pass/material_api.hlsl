#ifndef MAIN_MATERIAL_API_HLSL
#define MAIN_MATERIAL_API_HLSL

// ─────────────────────────────────────────────────────────────────────────────
// Движковый ПРОЛОГ material-API для main-пасса. Прячет всё движко-определённое, чтобы
// пользовательский surface содержал только `cbuffer MaterialBlock` + `getSurface`.
// Регистры материальных текстур — t2..t5 (слоты 0/1 заняты глобалками пасса: тень + env-кубмапа):
// albedo t2, normal t3, orm t4, emissive t5.
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
    TextureData textures[4];   // albedo, normal, orm, emissive — плотный UVL по required_slots
};

#define MATERIAL_BLOCK_REGISTER register(b1, space3)

// Регистры движковых storage-буферов базы зависят от числа сэмплеров пасса (shadercross
// нумерует storage ПОСЛЕ сэмплеров). Здесь 6 сэмплеров (shadow t0 + env t1 + albedo t2 +
// normal t3 + orm t4 + emissive t5) → LightBlock t6, ShadowCameras t7, Camera t8.
#define LIGHT_BLOCK_REGISTER     register(t6, space2)
#define SHADOW_CAMERAS_REGISTER  register(t7, space2)
#define CAMERA_REGISTER          register(t8, space2)

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
// ORM: возвращает сырой RGB (R=AO, G=Roughness, B=Metallic) — разбирает getSurface.
float3 SampleORM(float2 uv)
{
    return sampleAtlas(u_orm, u_ormSampler, textures[2].data, uv).rgb;
}
float3 SampleEmissive(float2 uv)
{
    return sampleAtlas(u_emissive, u_emissiveSampler, textures[3].data, uv).rgb;
}

#endif
