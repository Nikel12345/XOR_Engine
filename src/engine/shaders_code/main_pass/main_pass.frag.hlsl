#include "main_pass/math.hlsl"
#include "main_pass/lighting.hlsl"
#include "main_pass/shadowPCF.hlsl"

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);

// Env-кубмапа — глобалка пасса (как тень), слот 1. Заливается движком из skybox-креста
// (EngineContext::CreateCubemapFromCross). Объявлена в базе (общей для текстурного и
// текстурелесс прологов); прологи знают про этот сэмплер и сдвигают свои регистры.
[[vk::combinedImageSampler]]
TextureCube  u_envCube    : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState u_envSampler : register(s1, space2);

// Интерфейс окружения — единственная точка, знающая про источник. roughness → мип-LOD: гладкое
// зеркало (0) сэмплит детальный уровень, шероховатый металл (→1) — размытые мипы. Прим.: это
// дешёвая аппроксимация (обычные мипы, не prefiltered-IBL), но визуально размытие даёт.
float3 sampleEnv(float3 dir, float roughness)
{
    uint w, h, levels;
    u_envCube.GetDimensions(0, w, h, levels);
    return u_envCube.SampleLevel(u_envSampler, dir, saturate(roughness) * float(levels - 1)).rgb;
}

// Регистры заданы прологом (зависят от числа сэмплеров пасса: текстурный → t3/t4,
// текстурелесс → t1/t2). База включается ПОСЛЕ пролога — макросы уже определены.
StructuredBuffer<Light>       LightBlock    : LIGHT_BLOCK_REGISTER;
struct ShadowCamera { float4x4 view; float4x4 proj; };
StructuredBuffer<ShadowCamera> ShadowCameras : SHADOW_CAMERAS_REGISTER;

// Camera объявлена в ПРОЛОГЕ (material_api), чтобы getSurface мог посчитать view-вектор для POM
// до включения базы. Здесь она только используется (camPos = -R^T·t из view — rigid-inverse).

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

// Два выхода MRT: color (location 0) и emission (location 1). Раздельная эмиссия нужна,
// чтобы будущий bloom брал ИМЕННО светящееся, а не яркость вообще (тусклый glow должен
// блумить, освещённая стена — нет). Число выходов обязано совпадать с числом color-таргетов
// прохода (MAIN_PASS: scene_hdr + scene_emission).
struct PSOutput
{
    float4 color    : SV_Target0;   // линейный HDR-цвет сцены (свет + спекуляр + эмиссия)
    float4 emission : SV_Target1;   // только эмиссия — отдельный выход под bloom
};

PSOutput main(PSInput input, bool isFrontFace : SV_IsFrontFace)
{
    // Пользовательская часть: какова поверхность в этом пикселе (до освещения).
    SurfaceData surface = getSurface(input, isFrontFace);
    float3 n = surface.normal;

    // Спекуляр (Blinn-Phong): V на камеру, shininess из roughness. camPos = -R^T·t из view.
    float4x4 view      = Camera[0].view;
    // Трансляция view лежит в СТОЛБЦЕ 3 (_m03_m13_m23), не в строке 3 (та = 0,0,0,1).
    // camPos (мир) = -R^T·t — обратное rigid-преобразование (R = верхний 3x3, ортонормирован).
    float3   camPos    = -mul(transpose((float3x3)view), view._m03_m13_m23);
    float3   V         = normalize(camPos - input.v_worldPos);
    float    shininess = lerp(8.0, 256.0, saturate(1.0 - surface.roughness));
    // F0 — отражательность в лоб: диэлектрик ≈ 0.04 (4%), металл ≈ albedo. metallic интерполирует.
    // Благодаря этому спекуляр/отражение (и значит roughness) есть и у неметаллов, не только у металла.
    float3   F0        = lerp((float3)0.04, surface.baseColor, surface.metallic);
    float3   specSum   = float3(0.0, 0.0, 0.0);

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

        if (type == 2)
        {
            float3 L_dir     = normalize(-light.direction_angle.xyz);
            float  intensity = computeDirectionalLight(n, light, L_dir);
            float  dirShadow = 1.0;

            if (intensity > 0.0 && cameraOffset >= 0)
            {
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

                    float he_c       = 1.0 / ShadowCameras[slot].proj[0][0];
                    float far_c      = -1.0 / ShadowCameras[slot].proj[2][2];
                    float worldTexel = 2.0 * he_c / 1024.0;

                    float3 sampPos  = input.v_worldPos + n * (worldTexel * CSM_NORMAL_OFFSET);
                    float4 sampClip = worldToLightClip(sampPos, slot);
                    float  ndcZ     = sampClip.z / sampClip.w;

                    float biasNdc = (worldTexel * lerp(CSM_DEPTH_MIN, CSM_DEPTH_MAX, slope)) / far_c;

                    float shadow_d = computeShadowPCF(
                        u_shadowDepthArray, u_shadowSampler,
                        sampClip,
                        slot,
                        ndcZ - biasNdc);
                    dirShadow = shadow_d;
                    intensity *= shadow_d;
                }
            }

            if (intensity > 0.0)
                lightSum += light.color_power.rgb * intensity;

            // Спекуляр Blinn-Phong от directional — теперь для ВСЕХ поверхностей, сила = Френель(F0).
            // Энергонормировка (shininess+8)/8π: пик растёт с резкостью лоба, поэтому острый
            // блик (низкий roughness) пробивает ярким концентрированным глинтом, а не теряется.
            {
                float  NdotL_s = saturate(dot(n, L_dir));
                float3 HV      = L_dir + V;
                float  hl      = length(HV);
                // Защита от NaN: при L≈-V полувектор вырождается в ноль → normalize даёт NaN,
                // который через HDR/bloom расползается на весь кадр (мерцание). Гейтим спекуляр.
                if (NdotL_s > 0.0 && hl > 1e-4)
                {
                    float3 H     = HV / hl;
                    float  VdotH = saturate(dot(V, H));
                    float3 F     = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);   // Шлик: блик растёт к краям
                    float  norm  = (shininess + 8.0) / (8.0 * 3.14159265);
                    float  spec  = norm * pow(saturate(dot(n, H)), shininess);
                    specSum += light.color_power.rgb * (spec * NdotL_s * light.color_power.a * dirShadow) * F;
                }
            }
            continue;
        }

        float3 toLight = lightPos - input.v_worldPos;
        float  dist    = length(toLight);
        if (dist >= maxRange) continue;

        float3 L    = toLight / dist;
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

        // Спекуляр от спота/точки (тот же Френель-Blinn-Phong). intensity уже несёт NdotL,
        // затухание, конус и тень → маскирует блик так же, как диффуз.
        {
            float3 HV = L + V;
            float  hl = length(HV);
            if (hl > 1e-4)   // защита от NaN при L≈-V (см. directional выше)
            {
            float3 H     = HV / hl;
            float  VdotH = saturate(dot(V, H));
            float3 F     = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
            float  norm  = (shininess + 8.0) / (8.0 * 3.14159265);
            float  spec  = norm * pow(saturate(dot(n, H)), shininess);
            specSum += light.color_power.rgb * (spec * intensity * light.color_power.a) * F;
            }
        }
    }

    // Отражение окружения — для ВСЕХ поверхностей, сила = Френель по углу взгляда. F0 несёт
    // цвет: у металла env окрашивается в albedo, у диэлектрика — нейтральное небо, слабое в
    // лоб и сильнее к краям. roughness-aware (max(1-rough,F0)) гасит грязевой грань-блик на
    // шершавых. roughness уже размывает env через мип-LOD в sampleEnv.
    float  NdotV   = saturate(dot(n, V));
    float3 Fenv    = F0 + (max((float3)(1.0 - surface.roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    float3 R       = reflect(-V, n);
    float3 envRefl = sampleEnv(R, surface.roughness);
    specSum += envRefl * Fenv * surface.ao;   // AO гасит непрямое отражение

    // AO — затенение НЕпрямого света: модулирует только ambient-пол (и env выше), прямой свет нетронут.
    float3 lighting = max(lightSum, (float3)AMBIENT_LIGHT * surface.ao);
    // Диффуз гаснет на металле: проводник почти не имеет диффузного отражения, вся энергия — в зеркальном.
    float3 diffuse  = surface.baseColor * lighting * (1.0 - surface.metallic);
    float3 color    = diffuse + specSum + surface.emission;   // диффуз + спекуляр/отражение + эмиссия

    PSOutput o;
    o.color    = float4(color, surface.alpha);
    o.emission = float4(surface.emission, 1.0);
    return o;
}