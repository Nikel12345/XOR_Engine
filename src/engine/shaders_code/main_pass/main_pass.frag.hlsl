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

// Directional CSM: bias в МИРОВЫХ единицах через размер текселя каскада (he и far берём
// из его proj). Так acne одинаково отсутствует на всех каскадах и любом размере бокса,
// и нет peter-panning — в отличие от любого bias в нормированной глубине.
static const float CSM_NORMAL_OFFSET = 2.0;   // сдвиг приёмника вдоль нормали, в текселях
static const float CSM_DEPTH_MIN     = 0.75;  // глубинный bias (в текселях), прямой угол
static const float CSM_DEPTH_MAX     = 3.0;   // на скользящем угле

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

        // DIRECTIONAL: ни позиции, ни затухания — обрабатываем ДО toLight/dist,
        // иначе ранний `dist >= maxRange` его обрежет. maxRange здесь = far ortho-бокса.
        if (type == 2)
        {
            float3 L_dir     = normalize(-light.direction_angle.xyz);   // к свету
            float  intensity = computeDirectionalLight(n, light, L_dir);

            if (intensity > 0.0 && cameraOffset >= 0)
            {
                // Число каскадов лежит в position_radius.w (source_radius простаивает).
                // Боксы вложенные мелкий→крупный → берём ПЕРВЫЙ содержащий фрагмент
                // (самый резкий). Вхождение = ndc.xy в [-1,1] и ndc.z в [0,1].
                int    cascadeCount = max((int)light.position_radius.w, 1);
                int    slot       = -1;
                float4 chosenClip = float4(0.0, 0.0, 0.0, 1.0);
                for (int c = 0; c < cascadeCount; ++c)
                {
                    int    s    = cameraOffset + c;
                    float4 clip = worldToLightClip(input.v_worldPos, s);
                    float3 ndc  = clip.xyz / clip.w;
                    if (all(abs(ndc.xy) <= 1.0) && ndc.z >= 0.0 && ndc.z <= 1.0)
                    {
                        slot = s;
                        chosenClip = clip;
                        break;
                    }
                }

                if (slot >= 0)
                {
                    float NdotL_d = max(dot(n, L_dir), 0.0);
                    float slope   = saturate(1.0 - NdotL_d);

                    // Размер текселя и far этого каскада — из его ortho-proj (диагональ,
                    // к транспонированию устойчиво): he = 1/proj[0][0], far = -1/proj[2][2].
                    float he_c       = 1.0 / ShadowCameras[slot].proj[0][0];
                    float far_c      = -1.0 / ShadowCameras[slot].proj[2][2];
                    float worldTexel = 2.0 * he_c / 1024.0;

                    // Normal-offset: сдвигаем приёмник вдоль нормали на ~тексель — убирает
                    // acne геометрически, масштабируется текселем сам, без peter-panning.
                    float3 sampPos  = input.v_worldPos + n * (worldTexel * CSM_NORMAL_OFFSET);
                    float4 sampClip = worldToLightClip(sampPos, slot);
                    float  ndcZ     = sampClip.z / sampClip.w;

                    // Остаточный глубинный bias тоже в МИРОВЫХ единицах (тексель/far_c).
                    float biasNdc = (worldTexel * lerp(CSM_DEPTH_MIN, CSM_DEPTH_MAX, slope)) / far_c;

                    float shadow_d = computeShadowPCF(
                        u_shadowDepthArray, u_shadowSampler,
                        sampClip,
                        slot,
                        ndcZ - biasNdc);
                    intensity *= shadow_d;
                }
            }

            if (intensity > 0.0)
                lightSum += light.color_power.rgb * intensity;
            continue;
        }

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