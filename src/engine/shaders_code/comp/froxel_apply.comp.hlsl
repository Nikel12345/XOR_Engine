[[vk::combinedImageSampler]]
Texture2D<float>  u_depth                    : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_depthS                   : register(s0, space0);
[[vk::combinedImageSampler]]
Texture3D<float4> u_froxelVolumeAccumulated  : register(t1, space0);
[[vk::combinedImageSampler]]
SamplerState      u_froxelVolumeAccumulatedS : register(s1, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t2, space0);

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_scene : register(u0, space1);

#include "comp/view_from_depth.hlsli"
#include "comp/froxel_common.hlsli"

float3 DepthToWorld(float2 fragCoord, float depth)
{
    return InverseRigidTransform(Camera[0].view, ViewPos(fragCoord, depth));
}

[numthreads(32, 32, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    u_scene.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;

    float2 fragCoord = float2(tid.xy) / float2(width, height);
    float depth = u_depth.SampleLevel(u_depthS, fragCoord, 0);
    float3 worldPos = DepthToWorld(fragCoord, depth);
    float4x4 proj = Camera[0].proj;
    float3 froxelPos = GetFroxelUVW(worldPos, fog_far, proj, Camera[0].view, CameraNear(proj));
    float4 froxelLight = u_froxelVolumeAccumulated.SampleLevel(u_froxelVolumeAccumulatedS, froxelPos, 0);

    float4 light = u_scene[tid.xy];
    u_scene[tid.xy] = float4(light.rgb + froxelLight.rgb, 1.0);
}
