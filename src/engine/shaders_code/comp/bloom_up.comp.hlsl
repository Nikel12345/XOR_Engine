// Bloom upsample — 3×3 tent-фильтр, аддитивно поверх более крупного уровня (Jimenez / COD).
// Пирамида — ОДНА текстура с мипами: src = mip srcLod (мельче, уровень i+1), dst = mip i (крупнее).
// Читает src СЭМПЛЕРОМ (билинейка) на уровне srcLod, ПРИБАВЛЯЕТ к dst через storage RMW. dst и src —
// разные мипы одной текстуры (subresource'ы не пересекаются) → допустимо под флагом SIMULTANEOUS.

[[vk::combinedImageSampler]]
Texture2D<float4>   u_src     : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_sampler : register(s0, space0);

RWTexture2D<float4> u_dst : register(u0, space1);

cbuffer BloomParams : register(b0, space2) {
    uint  useKaris;
    float intensity;
    float threshold;
    float knee;
    uint  srcLod;      // mip-уровень источника (= уровень i+1)
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint dw, dh; u_dst.GetDimensions(dw, dh);            // размер mip-приёмника (i)
    if (tid.x >= dw || tid.y >= dh) return;

    uint sw, sh, nl; u_src.GetDimensions(srcLod, sw, sh, nl);  // размер mip-источника (i+1)
    float2 t  = 1.0 / float2(sw, sh);                    // радиус tent = 1 тексель ИСТОЧНИКА
    float2 uv = (float2(tid.xy) + 0.5) / float2(dw, dh);

    // tent 3×3:  1 2 1 / 2 4 2 / 1 2 1, нормировка /16
    float3 s  = u_src.SampleLevel(u_sampler, uv, srcLod).rgb * 4.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x, 0), srcLod).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x, 0), srcLod).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(0, -t.y), srcLod).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(0,  t.y), srcLod).rgb * 2.0;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x, -t.y), srcLod).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x, -t.y), srcLod).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2(-t.x,  t.y), srcLod).rgb;
    s += u_src.SampleLevel(u_sampler, uv + float2( t.x,  t.y), srcLod).rgb;
    s *= (1.0 / 16.0);

    u_dst[tid.xy] = float4(u_dst[tid.xy].rgb + s, 1.0);
}
