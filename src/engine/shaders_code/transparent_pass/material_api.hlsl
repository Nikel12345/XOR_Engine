#ifndef TRANSPARENT_MATERIAL_API_HLSL
#define TRANSPARENT_MATERIAL_API_HLSL

// Движковый ПРОЛОГ material-API для прозрачного пасса. Регистры материальных текстур — t0/t1
// (теней нет, глобалок на слоте 0 нет → сэмплеры с t0). Контракт PSInput/SurfaceData тот же,
// что в main-прологе (продублирован ниже). User-surface = `cbuffer MaterialBlock` + `getSurface`.

#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"

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
cbuffer TextureUVLBlock : register(b0, space3) {
    TextureData textures[2];   // albedo + normal — плотный UVL по required_slots
};

#define MATERIAL_BLOCK_REGISTER register(b1, space3)

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
