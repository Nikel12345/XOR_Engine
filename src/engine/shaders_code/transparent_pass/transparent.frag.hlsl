#include "main_pass/math.hlsl"
#include "main_pass/lighting.hlsl"

StructuredBuffer<Light> LightBlock : register(t2, space2);

static const float AMBIENT_LIGHT = 0.75;    // пол освещённости (как в main)

float4 main(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    // Пользовательская часть: какова поверхность в этом пикселе (до освещения).
    SurfaceData surface = getSurface(input, isFrontFace);
    float3 n = surface.normal;

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
    return float4(surface.baseColor * lighting, surface.alpha);
}
