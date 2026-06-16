#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"
#include "main_pass/lighting.hlsl"
#include "main_pass/shadowPCF.hlsl"

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);

[[vk::combinedImageSampler]]
Texture2DArray            u_albedo            : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState              u_albedoSampler     : register(s1, space2);

[[vk::combinedImageSampler]]
Texture2DArray            u_normal            : register(t2, space2);
[[vk::combinedImageSampler]]
SamplerState              u_normalSampler     : register(s2, space2);

StructuredBuffer<Light>       LightBlock    : register(t3, space2);
struct ShadowCamera { float4x4 view; float4x4 proj; };
StructuredBuffer<ShadowCamera> ShadowCameras : register(t4, space2);

struct TextureData { uint4 data; };
cbuffer TextureUVLBlock : register(b0, space3) {
    TextureData textures[4];
};

struct PSInput
{
    float4 sv_pos                               : SV_Position;
    [[vk::location(0)]] float2 v_uv             : TEXCOORD0;
    [[vk::location(1)]] float3 v_worldPos       : TEXCOORD1;
    [[vk::location(2)]] float3 v_worldNormal    : TEXCOORD2;
    [[vk::location(3)]] float3 v_worldTangent   : TEXCOORD3;
    [[vk::location(4)]] float3 v_worldBitangent : TEXCOORD4;
};

static const float AMBIENT_LIGHT = 0.75;    // пол освещённости (max с засветкой)

// Bias задаётся как ДОЛЯ расстояния до источника, а не абсолют: тексель карты в
// мире растёт линейно с дистанцией, поэтому контактная тень близких объектов
// остаётся плотной (без отрыва / peter-panning), а вдали bias успевает вырасти.
// Дополнительно растёт на скользящих углах, где глубина быстро меняется.
static const float SHADOW_BIAS_MIN = 0.002;   // прямой угол
static const float SHADOW_BIAS_MAX = 0.012;   // скользящий угол

float4 worldToLightClip(float3 worldPos, int slot)
{
    ShadowCamera cam = ShadowCameras[slot];
    return mul(cam.proj, mul(cam.view, float4(worldPos, 1.0)));
}

// refDist для distance shadow map: нормированное расстояние источник→фрагмент с bias.
float shadowRefDist(float dist, float maxRange, float NdotL)
{
    float d     = dist / maxRange;                 // [0..1]
    float slope = saturate(1.0 - NdotL);
    float bias  = d * lerp(SHADOW_BIAS_MIN, SHADOW_BIAS_MAX, slope);
    return d - bias;
}

float4 main(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[1].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);

    if (!isFrontFace) n = -n;

    float4 albedoSample = sampleAtlas(u_albedo, u_albedoSampler, textures[0].data, input.v_uv);
    float3 baseColor    = albedoSample.rgb;
    float  alpha        = albedoSample.a;

    float3 lightSum = float3(0.0, 0.0, 0.0);

    uint lightCount, stride;
    LightBlock.GetDimensions(lightCount, stride);

    for (uint i = 0; i < lightCount; ++i)
    {
        Light  light        = LightBlock[i];
        int    type         = light.light_info.x;
        int    cameraOffset = light.light_info.y;
        float  maxRange     = asfloat(light.light_info.z);
        float3 lightPos     = light.position_radius.xyz;

        float3 toLight = lightPos - input.v_worldPos;
        float  dist    = length(toLight);
        if (dist >= maxRange) continue;

        float3 L    = toLight / dist;          // от точки к источнику, нормализован
        float  NdotL = max(dot(n, L), 0.0);

        float intensity = 0.0;
        float shadow    = 1.0;

        if (type == 0)        // SPOT
        {
            intensity = computeSpotLight(n, light, L, dist, maxRange);
            if (intensity > 0.0 && cameraOffset >= 0)
                shadow = computeShadowPCF(
                    u_shadowDepthArray, u_shadowSampler,
                    worldToLightClip(input.v_worldPos, cameraOffset),
                    cameraOffset,
                    shadowRefDist(dist, maxRange, NdotL));
        }
        else if (type == 1)   // SPHERE / POINT
        {
            intensity = computePointLight(n, light, L, dist, maxRange);
            if (intensity > 0.0 && cameraOffset >= 0)
            {
                int slot = cameraOffset + getCubeFace(-toLight);
                shadow = computeShadowPCF(
                    u_shadowDepthArray, u_shadowSampler,
                    worldToLightClip(input.v_worldPos, slot),
                    slot,
                    shadowRefDist(dist, maxRange, NdotL));
            }
        }
        else continue;

        intensity *= shadow;
        if (intensity <= 0.0) continue;
        lightSum += light.color_power.rgb * intensity;
    }

    float3 lighting = max(lightSum, (float3)AMBIENT_LIGHT);
    float3 color    = baseColor * lighting;

    return float4(color, alpha);
}