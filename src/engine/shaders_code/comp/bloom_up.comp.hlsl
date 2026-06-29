// Bloom upsample — 3×3 tent-фильтр, аддитивно поверх более крупного уровня (Jimenez / COD).
// Читает src (мельче, уровень i+1) через LINEAR-сэмплер, ПРИБАВЛЯЕТ к dst (крупнее, уровень i)
// через storage read-modify-write. Идёт от самого мелкого мипа к крупному, накапливая размытие
// всех масштабов. dst в этом диспатче — только storage (RMW), src — только sampler → одна
// текстура не висит одновременно как sampler и storage.

[[vk::combinedImageSampler]]
Texture2D<float4>   u_src     : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_sampler : register(s0, space0);

RWTexture2D<float4> u_dst : register(u0, space1);

cbuffer BloomParams : register(b0, space2) {
    uint  useKaris;
    float intensity;
    float _pad0;
    float _pad1;
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint dw, dh; u_dst.GetDimensions(dw, dh);
    if (tid.x >= dw || tid.y >= dh) return;

    uint sw, sh; u_src.GetDimensions(sw, sh);
    float2 t  = 1.0 / float2(sw, sh);                       // радиус tent = 1 тексель ИСТОЧНИКА
    float2 uv = (float2(tid.xy) + 0.5) / float2(dw, dh);

    // tent 3×3:  1 2 1 / 2 4 2 / 1 2 1, нормировка /16
    float3 s  = u_src.SampleLevel(u_sampler, uv, 0).rgb * 4.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x, 0), 0).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x, 0), 0).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(0, -t.y), 0).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(0,  t.y), 0).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x, -t.y), 0).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x, -t.y), 0).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x,  t.y), 0).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x,  t.y), 0).rgb;
    s *= (1.0 / 16.0);

    u_dst[tid.xy] = float4(u_dst[tid.xy].rgb + s, 1.0);
}
