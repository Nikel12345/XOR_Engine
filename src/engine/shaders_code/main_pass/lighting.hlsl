#ifndef LIGHTING_HLSL
#define LIGHTING_HLSL

// ─────────────────────────────────────────────────────────────────────────────
// Голая модель света: Lambert N·L + затухание по расстоянию.
// Spot — угловой конус. Sphere — обычный точечный источник (радиус источника
// больше не используется). Без wrap-diffuse, edge-soft, source radius и т.п.
// ─────────────────────────────────────────────────────────────────────────────

struct Light {
    float4 position_radius;   // xyz = позиция; w = радиус источника (НЕ используется)
    float4 direction_angle;   // xyz = направление конуса; w = tan(полуугла)
    float4 color_power;       // rgb = цвет; a = мощность
    int4   light_info;        // x = тип; y = cameraOffset; z = asfloat(maxRange)
};

// Квадратичное затухание: гладко гаснет к границе maxRange.
float distanceAttenuation(float dist, float maxRange)
{
    float a = saturate(1.0 - dist / maxRange);
    return a * a;
}

// L    = нормализованное направление от точки к источнику (toLight / dist)
// dist = расстояние до источника
float computeSpotLight(float3 normal, Light light, float3 L, float dist, float maxRange)
{
    float3 spotDir = normalize(light.direction_angle.xyz);   // куда «смотрит» источник
    float  tanHalf = abs(light.direction_angle.w);
    float  cosHalf = rsqrt(1.0 + tanHalf * tanHalf);         // cos(полуугла конуса)

    // Косинус угла между осью конуса и направлением от источника к точке (-L).
    float cosFrag = dot(-L, spotDir);

    // Мягкая граница конуса в пределах небольшого угла у края.
    float cone = smoothstep(cosHalf, cosHalf + 0.02, cosFrag);
    if (cone <= 0.0) return 0.0;

    float NdotL = max(dot(normal, L), 0.0);
    float atten = distanceAttenuation(dist, maxRange);

    return NdotL * cone * atten * light.color_power.a;
}

float computePointLight(float3 normal, Light light, float3 L, float dist, float maxRange)
{
    float NdotL = max(dot(normal, L), 0.0);
    float atten = distanceAttenuation(dist, maxRange);

    return NdotL * atten * light.color_power.a;
}

#endif
