#include "main_pass/math.hlsl"
#include "main_pass/material.hlsl"
#include "main_pass/lighting.hlsl"

// Прозрачный пасс. Та же геометрия/VS, что у main, но БЕЗ карт теней — поэтому
// раскладка байндингов короче: сэмплеры идут первыми (t0..), затем storage-буферы.
//   t0/s0 — albedo, t1/s1 — normal (2 сэмплера) → LightBlock попадает на t2.
[[vk::combinedImageSampler]]
Texture2DArray u_albedo        : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerState   u_albedoSampler : register(s0, space2);

[[vk::combinedImageSampler]]
Texture2DArray u_normal        : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState   u_normalSampler : register(s1, space2);

StructuredBuffer<Light> LightBlock : register(t2, space2);

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

static const float AMBIENT_LIGHT = 0.75;    // пол освещённости (как в main)

float4 main(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float3 n = computeNormal(
        u_normal, u_normalSampler, textures[1].data,
        input.v_uv, input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);

    if (!isFrontFace) n = -n;

    // Прозрачность = альфа текстуры × per-material альфа (лежит в .w UVL слота альбедо,
    // см. RenderManager: туда пишутся биты Material::alpha). Блендинг включён в пайплайне.
    float4 albedoSample = sampleAtlas(u_albedo, u_albedoSampler, textures[0].data, input.v_uv);
    float3 baseColor    = albedoSample.rgb;
    float  materialAlpha = asfloat(textures[0].data.w);
    float  alpha        = albedoSample.a * materialAlpha;

    float3 lightSum = float3(0.0, 0.0, 0.0);

    uint lightCount, stride;
    LightBlock.GetDimensions(lightCount, stride);

    for (uint i = 0; i < lightCount; ++i)
    {
        Light  light    = LightBlock[i];
        int    type     = light.light_info.x;
        float  maxRange = asfloat(light.light_info.z);
        float3 lightPos = light.position_radius.xyz;

        float3 toLight = lightPos - input.v_worldPos;
        float  dist    = length(toLight);
        if (dist >= maxRange) continue;

        float3 L = toLight / dist;

        // Тени в прозрачном пассе не считаем (нет shadow-байндингов).
        float intensity = (type == 0)
            ? computeSpotLight(n, light, L, dist, maxRange)
            : computePointLight(n, light, L, dist, maxRange);

        if (intensity <= 0.0) continue;
        lightSum += light.color_power.rgb * intensity;
    }

    float3 lighting = max(lightSum, (float3)AMBIENT_LIGHT);
    return float4(baseColor * lighting, alpha);
}
