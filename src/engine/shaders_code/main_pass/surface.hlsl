#ifndef SURFACE_HLSL
#define SURFACE_HLSL

#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"


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

[[vk::combinedImageSampler]]
Texture2DArray            u_albedo            : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState              u_albedoSampler     : register(s1, space2);

[[vk::combinedImageSampler]]
Texture2DArray            u_normal            : register(t2, space2);
[[vk::combinedImageSampler]]
SamplerState              u_normalSampler     : register(s2, space2);

struct TextureData { uint4 data; };
cbuffer TextureUVLBlock : register(b0, space3) {
    TextureData textures[2];   // albedo + normal — плотный UVL по required_slots
};

// Per-material факторы (дублирует OpaqueMaterialParams в C++). Активен baseColorFactor (тинт);
// metallic/roughness/emission — задел (нужен PBR-свет), пока в шейдере не читаются.
cbuffer MaterialBlock : register(b1, space3) {
    float4 baseColorFactor;
    // float metallic; float roughness; float3 emissive; float emissiveStrength;
};


struct SurfaceData
{
    float3 baseColor;
    float3 normal;
    float  alpha;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    SurfaceData s;

    s.normal = computeNormal(
        u_normal, u_normalSampler, textures[1].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    if (!isFrontFace) s.normal = -s.normal;

    float4 albedoSample = sampleAtlas(u_albedo, u_albedoSampler, textures[0].data, input.v_uv);
    s.baseColor = albedoSample.rgb * baseColorFactor.rgb;   // × per-material тинт (дефолт белый)
    s.alpha     = albedoSample.a * input.v_alpha;           // × per-instance (opaque-пасс альфу игнорит)

    return s;
}

#endif
