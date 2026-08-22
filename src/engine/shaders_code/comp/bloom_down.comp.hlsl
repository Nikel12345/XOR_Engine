// Bloom downsample — 13-тап фильтр (Jimenez / Call of Duty: Advanced Warfare, SIGGRAPH 2014).
// Читает src (вдвое больший уровень) через combinedImageSampler с LINEAR-фильтром, пишет dst
// (текущий уровень) через storage. Размеры берём из самих текстур → шейдер не зависит от уровня.
// На ПЕРВОМ downsample (useKaris=1) включается Karis-average — гасит firefly от одиночных
// сверхъярких пикселей эмиссии. Дальше Karis не нужен.

[[vk::combinedImageSampler]]
Texture2D<float4>   u_src     : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_sampler : register(s0, space0);

// [[vk::image_format]]: DXC не выводит формат storage-образа из float4 (молча даёт rgba32f) —
// формат обязан совпадать с текстурой (rgba16f, см. TexturesPresets::BloomLevel), иначе UB.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_dst : register(u0, space1);

cbuffer BloomParams : register(b0, space2) {
    uint  useKaris;
    float intensity;   // не используется здесь
    float threshold;
    float knee;
    float clampMax;   // потолок вклада сцены (prefilter); в down/up/composite не используется
};

// Пирамида — ОТДЕЛЬНАЯ текстура на уровень: src = уровень i-1 (вдвое крупнее, sampled),
// dst = уровень i (storage). Разные текстуры → никакого одновременного RW+sampled одной.
float3 S(float2 uv) { return u_src.SampleLevel(u_sampler, uv, 0).rgb; }
float  karis(float3 c) { float l = dot(c, float3(0.2126, 0.7152, 0.0722)); return 1.0 / (1.0 + l); }

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint dw, dh; u_dst.GetDimensions(dw, dh);
    if (tid.x >= dw || tid.y >= dh) return;

    uint sw, sh; u_src.GetDimensions(sw, sh);
    float2 t  = 1.0 / float2(sw, sh);                       // шаг = 1 тексель ИСТОЧНИКА
    float2 uv = (float2(tid.xy) + 0.5) / float2(dw, dh);

    // 13 выборок: внешняя сетка 3×3 (a..i, шаг ±2) + центральная 2×2 (j..m, шаг ±1).
    float3 a = S(uv + t * float2(-2, -2));
    float3 b = S(uv + t * float2( 0, -2));
    float3 c = S(uv + t * float2( 2, -2));
    float3 d = S(uv + t * float2(-2,  0));
    float3 e = S(uv + t * float2( 0,  0));
    float3 f = S(uv + t * float2( 2,  0));
    float3 g = S(uv + t * float2(-2,  2));
    float3 h = S(uv + t * float2( 0,  2));
    float3 i = S(uv + t * float2( 2,  2));
    float3 j = S(uv + t * float2(-1, -1));
    float3 k = S(uv + t * float2( 1, -1));
    float3 l = S(uv + t * float2(-1,  1));
    float3 m = S(uv + t * float2( 1,  1));

    float3 result;
    if (useKaris != 0)
    {
        // Пять групп 2×2, каждая взвешена своим karis-весом (тусклые группы — больше веса).
        float3 g0 = (j + k + l + m) * 0.25;   // центр
        float3 g1 = (a + b + d + e) * 0.25;
        float3 g2 = (b + c + e + f) * 0.25;
        float3 g3 = (d + e + g + h) * 0.25;
        float3 g4 = (e + f + h + i) * 0.25;
        float w0 = karis(g0) * 0.5;
        float w1 = karis(g1) * 0.125;
        float w2 = karis(g2) * 0.125;
        float w3 = karis(g3) * 0.125;
        float w4 = karis(g4) * 0.125;
        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / max(w0 + w1 + w2 + w3 + w4, 1e-5);
    }
    else
    {
        result  = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    u_dst[tid.xy] = float4(result, 1.0);
}
