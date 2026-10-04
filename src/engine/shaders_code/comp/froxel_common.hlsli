#ifndef FROXEL_COMMON_HLSLI
#define FROXEL_COMMON_HLSLI

// По мотивам froxel fog из Luz (github.com/hadryansalles/Luz, MIT). Требует объявленным до
// включения StructuredBuffer<CameraData> Camera.

static const float PI = 3.14159265359;

cbuffer FroxelFogParams : register(b0, space2) {
    float3 fog_albedo;
    float  fog_far;
    float  fog_density;
    float  fog_scattering;
    float  fog_absorption;
    float  fog_anisotropy;
    float  fog_intensity;
    uint   fog_samples;
    uint   light_count;
};

float CameraNear(float4x4 proj)
{
    return proj[2][3] / (proj[2][2] - 1.0);
}

float3 InverseRigidTransform(float4x4 view, float3 viewPos)
{
    return mul(transpose((float3x3)view), viewPos - view._m03_m13_m23);
}

float3 GetFroxelUVW(float3 froxelPos, float froxelVolumeDepth, float4x4 proj, float4x4 view, float camNear)
{
    float4 viewPos = mul(view, float4(froxelPos, 1.0));
    float4 clipPos = mul(proj, viewPos);
    clipPos /= clipPos.w;
    float2 uv = clipPos.xy * 0.5 + 0.5;
    float linearDepth = -viewPos.z;
    float w = (linearDepth - camNear) / (froxelVolumeDepth - camNear);
    return clamp(float3(uv, w), 0.0, 1.0);
}

#endif
