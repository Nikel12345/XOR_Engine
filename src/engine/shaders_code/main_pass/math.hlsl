#ifndef MATH_HLSL
#define MATH_HLSL

float2 unpackUnorm2x16(uint p)
{
    return float2(
        float(p & 0xFFFFu) / 65535.0,
        float(p >> 16u)    / 65535.0
    );
}

float3 TransformOrigin(float4x4 m)
{
    return float3(m[0][3], m[1][3], m[2][3]);
}

float TransformMeanScale(float4x4 m)
{
    return (length(float3(m[0][0], m[1][0], m[2][0])) +
            length(float3(m[0][1], m[1][1], m[2][1])) +
            length(float3(m[0][2], m[1][2], m[2][2]))) / 3.0;
}

#endif