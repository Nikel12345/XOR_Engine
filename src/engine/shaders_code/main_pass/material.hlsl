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
    float3 n = sampleAtlas(normalMap, normalSampler, normalTextureData, uv).rgb * 2.0 - 1.0;
    n.x = -n.x;
    return normalize(mul(n, TBN));
}

#endif