#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "main_pass/math.hlsl"

// Сэмплирование атласной текстуры.
// textureData: x = packed offset (unorm2x16), y = packed scale (unorm2x16), z = layer
float4 sampleAtlas(
    Texture2DArray atlas,
    SamplerState   samp,
    uint4          textureData,
    float2         uv)
{
    float2 offset = unpackUnorm2x16(textureData.x);
    float2 scale  = unpackUnorm2x16(textureData.y);
    uint   layer  = textureData.z;
    return atlas.Sample(samp, float3(uv * scale + offset, float(layer)));
}

// Чтение нормали из normal map (атлас) и перевод в world space через TBN.
float3 computeNormal(
    Texture2DArray normalMap,
    SamplerState   normalSampler,
    uint4          normalTextureData,
    float2         uv,
    float3         worldTangent,
    float3         worldBitangent,
    float3         worldNormal)
{
    float3x3 TBN = float3x3(
        worldTangent,
        worldBitangent,
        normalize(worldNormal)
    );
    // Нормаль-маппинг. Освещение нелинейно по нормали, а фильтрация сглаживает лишь ВЕКТОР нормали
    // → высокочастотная нормаль недосэмпливается и её шейдинг мерцает при движении камеры (тёмное
    // дрожание, на тёплом дереве читается как цветовой сдвиг). Полностью убрать субпиксельное
    // мерцание при сильном рельефе можно только TAA; здесь — компромисс двумя ручками:
    //   NORMAL_STRENGTH — сила рельефа. Больше = выразительнее, но заметнее мерцает.
    //   NORMAL_MIP_BIAS — префильтр (блюр нормали, +к авто-LOD). Больше = меньше мерцания, мягче
    //                     рельеф. Работает ТОЛЬКО с aniso=OFF у DEFAULT_SAMPLER (иначе резкая ось
    //                     футпринта не даёт мипу префильтровать) — см. TextureSamplerPresets.h.
    const float NORMAL_STRENGTH = 0.7;
    const float NORMAL_MIP_BIAS = 1.0;
    float2 noff = unpackUnorm2x16(normalTextureData.x);
    float2 nsc  = unpackUnorm2x16(normalTextureData.y);
    float3 n = normalMap.SampleBias(normalSampler, float3(uv * nsc + noff, float(normalTextureData.z)), NORMAL_MIP_BIAS).rgb * 2.0 - 1.0;
    n.xy *= NORMAL_STRENGTH;
    return normalize(mul(n, TBN));
}

#endif