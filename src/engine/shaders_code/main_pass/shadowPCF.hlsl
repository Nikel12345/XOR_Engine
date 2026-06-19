#ifndef SHADOW_PCF_HLSL
#define SHADOW_PCF_HLSL

int getCubeFace(float3 dir)
{
    float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.x && a.y >= a.z) return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

static const int PCF_RADIUS = 1;   // 1 => 3x3 выборки


float computeShadowPCF(
    Texture2DArray<float>  depthArray,
    SamplerComparisonState shadowSampler,
    float4                 lightClipPos,
    int                    slot,
    float                  refDist)
{
    if (lightClipPos.w <= 0.0) return 1.0;

    float3 ndc = lightClipPos.xyz / lightClipPos.w;
    float2 uv  = float2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));

    // Вне фрустума источника или дальше far — тень не накладываем.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || refDist > 1.0)
        return 1.0;

    float w, h, layers;
    depthArray.GetDimensions(w, h, layers);
    float2 texelSize = 1.0 / float2(w, h);

    float sum   = 0.0;
    float count = 0.0;
    [unroll]
    for (int y = -PCF_RADIUS; y <= PCF_RADIUS; ++y)
    {
        [unroll]
        for (int x = -PCF_RADIUS; x <= PCF_RADIUS; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            sum += depthArray.SampleCmpLevelZero(
                shadowSampler,
                float3(uv + offset, float(slot)),
                refDist);
            count += 1.0;
        }
    }

    return sum / count;
}

#endif
