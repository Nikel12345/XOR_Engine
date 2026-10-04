[[vk::combinedImageSampler]]
Texture3D<float4> u_froxelVolume  : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_froxelVolumeS : register(s0, space0);

[[vk::image_format("rgba16f")]]
RWTexture3D<float4> u_froxelVolumeAccumulated : register(u0, space1);

// От камеры вглубь: свечение слоя гасит только туман перед ним. В Luz порядок обратный.
float4 Accumulate(float4 accumulated, float4 slice)
{
    float3 light = accumulated.rgb + saturate(exp(-accumulated.a)) * slice.rgb;
    return float4(light, accumulated.a + slice.a);
}

void Write(int3 pixelPos, float4 value)
{
    u_froxelVolumeAccumulated[pixelPos] = float4(value.rgb, exp(-value.a));
}

[numthreads(32, 32, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint sizeX, sizeY, sizeZ;
    u_froxelVolumeAccumulated.GetDimensions(sizeX, sizeY, sizeZ);
    if (tid.x >= sizeX || tid.y >= sizeY) return;
    int2 gridPos = int2(tid.xy);

    float4 current = u_froxelVolume.Load(int4(gridPos, 0, 0));
    Write(int3(gridPos, 0), current);

    for (int i = 1; i < int(sizeZ); i++) {
        float4 next = u_froxelVolume.Load(int4(gridPos, i, 0));
        current = Accumulate(current, next);
        Write(int3(gridPos, i), current);
    }
}
