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

static const float NORMAL_BIAS              = 0.1;
static const float VOLUME_BOUNDARY_SOFTNESS = 1.1;

float4 worldToLightClip(float3 worldPos, float3 geoNormal, int slot, float normalBias)
{
    float3 p = worldPos + geoNormal * normalBias;
    ShadowCamera cam = ShadowCameras[slot];
    return mul(cam.proj, mul(cam.view, float4(p, 1.0)));
}

float4 main(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[1].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);

    if (!isFrontFace) n = -n;

    float3 geoN = normalize(input.v_worldNormal);

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

        float intensity = 0.0;

        if (type == 0)
        {
            // передаём уже готовые toLight и dist
            intensity = computeSpotLight(input.v_worldPos, n, light, toLight, dist);
            if (intensity > 0.0)
            {
                float ndotl     = max(dot(n, toLight / dist), 0.0);
                float ndotlFade = smoothstep(0.05, 0.15, ndotl);
                intensity *= ndotlFade;
                if (cameraOffset >= 0 && intensity > 0.0)
                    intensity *= computeShadowPCF(
                        u_shadowDepthArray, u_shadowSampler,
                        worldToLightClip(input.v_worldPos, geoN, cameraOffset, NORMAL_BIAS),
                        cameraOffset,
                        input.sv_pos.xy);
            }
        }
        else if (type == 1)
        {
            // передаём уже готовые toLight и dist
            intensity = computeSphereLight(input.v_worldPos, n, light, toLight, dist);
            if (cameraOffset >= 0 && intensity > 0.0)
            {
                float R = light.position_radius.w;
                if (dist > R)
                {
                    int   face  = getCubeFace(-toLight);
                    int   slot  = cameraOffset + face;
                    float ndotl = max(dot(n, toLight / dist), 0.0);

                    float pcf = computeShadowPCF(
                        u_shadowDepthArray, u_shadowSampler,
                        worldToLightClip(input.v_worldPos, geoN, slot, NORMAL_BIAS),
                        slot,
                        input.sv_pos.xy);

                    float vf = (VOLUME_BOUNDARY_SOFTNESS > 1.0)
                        ? smoothstep(R, R * VOLUME_BOUNDARY_SOFTNESS, dist) : 1.0;

                    intensity *= lerp(1.0, pcf, vf) * lerp(1.0, smoothstep(0.05, 0.15, ndotl), vf);
                }
            }
        }
        else continue;

        if (intensity <= 0.0) continue;
        lightSum += light.color_power.rgb * intensity;
    }

    float3 lighting = max(lightSum, (float3)AMBIENT);
    float3 color    = baseColor * lighting;

    return float4(color, alpha);
}