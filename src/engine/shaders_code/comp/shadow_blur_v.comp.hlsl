[[vk::combinedImageSampler]]
Texture2D<float2> u_input   : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_sampler : register(s0, space0);

RWTexture2D<float2> u_output : register(u0, space1);

static const float weights[5] = { 0.20416, 0.18017, 0.12382, 0.06629, 0.02762 };

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h, levels;
    u_input.GetDimensions(0, w, h, levels);

    if (tid.x >= w || tid.y >= h) return;

    float2 uv    = (float2(tid.xy) + 0.5) / float2(w, h);
    float  texel = 1.0 / float(h);

    float2 result = u_input.SampleLevel(u_sampler, uv, 0) * weights[0];

    [unroll]
    for (int i = 1; i < 5; ++i) {
        float2 off = float2(0, texel * i);
        result += u_input.SampleLevel(u_sampler, uv + off, 0) * weights[i];
        result += u_input.SampleLevel(u_sampler, uv - off, 0) * weights[i];
    }

    u_output[tid.xy] = result;
}