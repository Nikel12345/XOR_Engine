// У воздуха нет нормали: свет в тумане — модель main_pass без Lambert.
#define LIGHT_IGNORES_NORMAL
#include "main_pass/lighting.hlsl"
#include "main_pass/shadowPCF.hlsl"

[[vk::combinedImageSampler]]
Texture2DArray<float>  u_shadowDepthArray : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerComparisonState u_shadowSampler    : register(s0, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData>   Camera        : register(t1, space0);
StructuredBuffer<Light>        LightBlock    : register(t2, space0);
struct ShadowCamera { float4x4 view; float4x4 proj; };
StructuredBuffer<ShadowCamera> ShadowCameras : register(t3, space0);

[[vk::image_format("rgba16f")]]
RWTexture3D<float4> u_froxelVolume : register(u0, space1);

#include "comp/froxel_common.hlsli"

float PhaseFunction(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

float4 WorldToShadowClip(float3 worldPos, int slot)
{
    ShadowCamera cam = ShadowCameras[slot];
    return mul(cam.proj, mul(cam.view, float4(worldPos, 1.0)));
}

float ShadowCompare(float4 lightClipPos, int slot, float refDist)
{
    if (lightClipPos.w <= 0.0) return 1.0;
    float3 ndc = lightClipPos.xyz / lightClipPos.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || refDist > 1.0) return 1.0;
    return u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(uv, float(slot)), refDist);
}

// Как в Luz: 1 — точка в тени, 0 — освещена. Выбор слоя теневого массива — как в main_pass.
float SampleShadowMap(Light light, float3 fragPos)
{
    int type = light.light_info.x;
    int cameraOffset = light.light_info.y;
    if (cameraOffset < 0) return 0.0;

    if (type == 2) {
        int cascadeCount = max((int)light.position_radius.w, 1);
        for (int c = 0; c < cascadeCount; ++c) {
            int slot = cameraOffset + c;
            float4 clip = WorldToShadowClip(fragPos, slot);
            float3 ndc = clip.xyz / clip.w;
            if (all(abs(ndc.xy) <= 1.0) && ndc.z >= 0.0 && ndc.z <= 1.0)
                return 1.0 - ShadowCompare(clip, slot, ndc.z);
        }
        return 0.0;
    }

    float3 lightToFrag = fragPos - light.position_radius.xyz;
    float refDist = length(lightToFrag) / asfloat(light.light_info.z);
    int slot = (type == 1) ? cameraOffset + getCubeFace(lightToFrag) : cameraOffset;
    return 1.0 - ShadowCompare(WorldToShadowClip(fragPos, slot), slot, refDist);
}

// depthOffset — сдвиг точки вдоль глубины в долях ячейки, [-0.5, 0.5]; 0 — центр.
float3 GetFroxelPos(int3 froxelIndex, float3 froxelVolumeSize, float froxelVolumeDepth, float4x4 proj, float4x4 view, float camNear, float depthOffset)
{
    float3 uvw = (float3(froxelIndex) + 0.5 + float3(0.0, 0.0, depthOffset)) / froxelVolumeSize;

    float2 ndcXY = uvw.xy * 2.0 - 1.0;
    float linearDepth = camNear + uvw.z * (froxelVolumeDepth - camNear);
    float viewZ = -linearDepth;
    float viewX = ndcXY.x * -viewZ / proj[0][0];
    float viewY = ndcXY.y * -viewZ / proj[1][1];

    return InverseRigidTransform(view, float3(viewX, viewY, viewZ));
}

float GetLayerThickness(int3 froxelIndex, float3 froxelVolumeSize, float zFar, float4x4 proj, float4x4 view, float camNear)
{
    float3 current = GetFroxelPos(froxelIndex, froxelVolumeSize, zFar, proj, view, camNear, 0.0);

    if (froxelIndex.z == 0) {
        float3 next = GetFroxelPos(froxelIndex + int3(0, 0, 1), froxelVolumeSize, zFar, proj, view, camNear, 0.0);
        return length(next - current);
    }

    if (froxelIndex.z == int(froxelVolumeSize.z) - 1) {
        float3 prev = GetFroxelPos(froxelIndex - int3(0, 0, 1), froxelVolumeSize, zFar, proj, view, camNear, 0.0);
        return length(current - prev);
    }

    float3 prev = GetFroxelPos(froxelIndex - int3(0, 0, 1), froxelVolumeSize, zFar, proj, view, camNear, 0.0);
    float3 next = GetFroxelPos(froxelIndex + int3(0, 0, 1), froxelVolumeSize, zFar, proj, view, camNear, 0.0);
    float thicknessToPrev = length(current - prev);
    float thicknessToNext = length(next - current);
    return (thicknessToPrev + thicknessToNext) * 0.5;
}

float3 InScatteredLight(float3 worldPos, float3 toCamera)
{
    float3 lighting = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < light_count; ++i) {
        Light light = LightBlock[i];
        int type = light.light_info.x;
        float3 toLight;
        float intensity;

        if (type == 2) {
            toLight = normalize(-light.direction_angle.xyz);
            intensity = computeDirectionalLight((float3)0, light, toLight);
        } else {
            float3 fragToLight = light.position_radius.xyz - worldPos;
            float dist = length(fragToLight);
            float maxRange = asfloat(light.light_info.z);
            if (dist >= maxRange) continue;
            toLight = fragToLight / dist;

            if (type == 0)      intensity = computeSpotLight((float3)0, light, toLight, dist, maxRange);
            else if (type == 1) intensity = computePointLight((float3)0, light, toLight, dist, maxRange);
            else continue;
        }
        if (intensity <= 0.0) continue;

        // Угол рассеяния — между направлением, куда идёт свет (-toLight), и направлением на камеру:
        // пик при g > 0, когда смотришь на источник. У Luz у точечных и спотов знак обратный.
        float cosTheta = dot(-toLight, toCamera);
        lighting += light.color_power.rgb * intensity * PhaseFunction(cosTheta, fog_anisotropy) * (1.0 - SampleShadowMap(light, worldPos));
    }

    return lighting;
}

[numthreads(10, 10, 8)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint sizeX, sizeY, sizeZ;
    u_froxelVolume.GetDimensions(sizeX, sizeY, sizeZ);
    float3 froxelVolumeSize = float3(sizeX, sizeY, sizeZ);
    if (tid.x >= sizeX || tid.y >= sizeY || tid.z >= sizeZ) return;
    int3 gridPos = int3(tid);

    float4x4 view = Camera[0].view;
    float4x4 proj = Camera[0].proj;
    float camNear = CameraNear(proj);

    float layerThickness = GetLayerThickness(gridPos, froxelVolumeSize, fog_far, proj, view, camNear);

    float dustDensity = fog_density;

    float scattering = fog_scattering * dustDensity * layerThickness;
    float absorption = fog_absorption * dustDensity * layerThickness;

    float3 cameraPos = InverseRigidTransform(view, float3(0.0, 0.0, 0.0));

    uint sampleCount = max(fog_samples, 1u);
    float3 lighting = float3(0.0, 0.0, 0.0);
    for (uint s = 0; s < sampleCount; ++s) {
        float depthOffset = (float(s) + 0.5) / float(sampleCount) - 0.5;
        float3 worldPos = GetFroxelPos(gridPos, froxelVolumeSize, fog_far, proj, view, camNear, depthOffset);
        lighting += InScatteredLight(worldPos, normalize(cameraPos - worldPos));
    }
    lighting /= float(sampleCount);

    lighting *= fog_albedo * fog_intensity;

    u_froxelVolume[tid] = float4(lighting * scattering, scattering + absorption);
}
