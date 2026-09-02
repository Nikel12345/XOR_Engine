// Билатеральный блюр AO, ВЕРТИКАЛЬНЫЙ шаг: __ssao_temp → __ssao. Возвращает результат в ту же
// текстуру, которую читает композит. Тело — общее (ssao_blur.hlsli), отличие от горизонтального
// только в оси и в том, какая текстура источник.

[[vk::combinedImageSampler]]
Texture2D<float>  u_src    : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_srcS   : register(s0, space0);
[[vk::combinedImageSampler]]
Texture2D<float>  u_depth  : register(t1, space0);
[[vk::combinedImageSampler]]
SamplerState      u_depthS : register(s1, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t2, space0);   // ro-буфер идёт ПОСЛЕ сэмплеров

[[vk::image_format("r32f")]]
RWTexture2D<float> u_dst : register(u0, space1);

cbuffer AOParams : register(b0, space2) {
    float radius;      // блюру отсюда нужен только он — порог билатерального веса
    float intensity;
    float power;
    float bias;
};

#include "comp/ssao_blur.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    SsaoBlurAxis(tid.xy, float2(0.0, 1.0));
}
