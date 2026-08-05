#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"
#include "main_pass/lighting.hlsl"
#include "main_pass/shadowPCF.hlsl"

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);


[[vk::combinedImageSampler]]
Texture2DArray          u_albedo         : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState            u_albedoSampler  : register(s1, space2);

[[vk::combinedImageSampler]]
Texture2DArray          u_normal         : register(t2, space2);
[[vk::combinedImageSampler]]
SamplerState            u_normalSampler  : register(s2, space2);

StructuredBuffer<Light> LightBlock : register(t3, space2);

struct TextureData {
    uint4 data;
};

cbuffer TextureUVLBlock : register(b0, space3) {
    TextureData textures[4];
};

struct PSInput
{
    float4                                       sv_pos          : SV_Position;
    [[vk::location(0)]]  float2 v_uv             : TEXCOORD0;
    [[vk::location(1)]]  float3 v_worldPos       : TEXCOORD1;
    [[vk::location(2)]]  float3 v_worldNormal    : TEXCOORD2;
    [[vk::location(3)]]  float3 v_worldTangent   : TEXCOORD3;
    [[vk::location(4)]]  float3 v_worldBitangent : TEXCOORD4;
    [[vk::location(5)]]  float4 v_lsp0           : TEXCOORD5;
    [[vk::location(6)]]  float4 v_lsp1           : TEXCOORD6;
    [[vk::location(7)]]  float4 v_lsp2           : TEXCOORD7;
    [[vk::location(8)]]  float4 v_lsp3           : TEXCOORD8;
    [[vk::location(9)]]  float4 v_lsp4           : TEXCOORD9;
    [[vk::location(10)]] float4 v_lsp5           : TEXCOORD10;
};

// HLSL не поддерживает динамическую индексацию выходов/входов — разворачиваем вручную
float4 getLightSpacePos(PSInput input, int i)
{
    switch (i)
    {
        case 0:  return input.v_lsp0;
        case 1:  return input.v_lsp1;
        case 2:  return input.v_lsp2;
        case 3:  return input.v_lsp3;
        case 4:  return input.v_lsp4;
        default: return input.v_lsp5;
    }
}
static const float VOLUME_BOUNDARY_SOFTNESS = 1.1;

float4 main(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float3 ng = normalize(input.v_worldNormal);
    float dev = 1.0 - abs(ng.y);          // 0 если нормаль вертикальна, >0 если наклонена
    return float4(saturate(dev * 10.0).xxx, 1.0);
}