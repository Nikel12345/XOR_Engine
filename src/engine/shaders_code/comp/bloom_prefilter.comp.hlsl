// Bloom prefilter — НУЛЕВОЙ уровень пирамиды. Собирает источник свечения из ДВУХ текстур:
//   bright = softKnee(scene_hdr) + scene_emission
// scene_hdr даёт физически яркое (HDR-спекуляр, пересвет, свет в камеру) ВЫШЕ порога; эмиссия
// подмешивается всегда (художник пометил объект светящимся независимо от яркости). Дальше — тот же
// 13-тап даунсемпл с Karis-average, что и в bloom_down (level 0, useKaris всегда), гасящий fireflies
// от одиночных сверхъярких текселей острого глинта. Читает обе текстуры LINEAR-сэмплером (апскейл-фри:
// scene_* полного размера → bloom_0 половинного), пишет bloom_0 через storage.

[[vk::combinedImageSampler]]
Texture2D<float4>   u_scene    : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_sceneS   : register(s0, space0);
[[vk::combinedImageSampler]]
Texture2D<float4>   u_emission : register(t1, space0);
[[vk::combinedImageSampler]]
SamplerState        u_emissionS: register(s1, space0);

// [[vk::image_format]]: DXC не выводит формат storage-образа из float4 (молча даёт rgba32f) —
// формат обязан совпадать с текстурой (rgba16f, см. TexturesPresets::BloomLevel), иначе UB.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_dst : register(u0, space1);

cbuffer BloomParams : register(b0, space2) {
    uint  useKaris;    // 1 — Karis-average (гасит firefly'и), 0 — обычные веса Jimenez
    float intensity;   // здесь = масштаб вклада СЦЕНЫ (блик/пересвет); эмиссию НЕ трогает
    float threshold;   // порог яркости сцены
    float knee;        // ширина мягкого колена
};

// Одностороннее колено: НИЖЕ threshold — строго ноль (обычный диффуз ≤1 не блумит вообще),
// в полосе [threshold, threshold+knee] — гладкий разгон (smoothstep), выше — полный цвет.
// threshold здесь = настоящий ПОЛ: ярче него только спекуляр-глинт и пересвет. Возвращаем scene,
// домноженный на gate, поэтому хью сохраняется (масштабируем весь цвет, а не каналы по отдельности).
float3 softKnee(float3 c)
{
    float br   = max(c.r, max(c.g, c.b));          // яркость = макс канал
    float gate = smoothstep(threshold, threshold + max(knee, 1e-5), br);
    return c * gate;
}

// Источник свечения в точке uv: яркая часть сцены + полная эмиссия.
float3 B(float2 uv)
{
    float3 hdr = u_scene.SampleLevel(u_sceneS, uv, 0).rgb;
    float3 emi = u_emission.SampleLevel(u_emissionS, uv, 0).rgb;
    // Вклад сцены масштабируем (блик металла очень концентрированный → даёт широкое яркое гало).
    // Меньше энергии в пирамиде → и тусклее, и компактнее гало. Эмиссия идёт в полную силу.
    return softKnee(hdr) * intensity + emi;
}

float karis(float3 c) { float l = dot(c, float3(0.2126, 0.7152, 0.0722)); return 1.0 / (1.0 + l); }

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint dw, dh; u_dst.GetDimensions(dw, dh);
    if (tid.x >= dw || tid.y >= dh) return;

    uint sw, sh; u_scene.GetDimensions(sw, sh);
    float2 t  = 1.0 / float2(sw, sh);                       // шаг = 1 тексель ИСТОЧНИКА
    float2 uv = (float2(tid.xy) + 0.5) / float2(dw, dh);

    // 13 выборок: внешняя сетка 3×3 (a..i, шаг ±2) + центральная 2×2 (j..m, шаг ±1).
    float3 a = B(uv + t * float2(-2, -2));
    float3 b = B(uv + t * float2( 0, -2));
    float3 c = B(uv + t * float2( 2, -2));
    float3 d = B(uv + t * float2(-2,  0));
    float3 e = B(uv + t * float2( 0,  0));
    float3 f = B(uv + t * float2( 2,  0));
    float3 g = B(uv + t * float2(-2,  2));
    float3 h = B(uv + t * float2( 0,  2));
    float3 i = B(uv + t * float2( 2,  2));
    float3 j = B(uv + t * float2(-1, -1));
    float3 k = B(uv + t * float2( 1, -1));
    float3 l = B(uv + t * float2(-1,  1));
    float3 m = B(uv + t * float2( 1,  1));

    float3 result;
    if (useKaris != 0)
    {
        // Karis-average (пять групп 2×2, тусклые группы — больше веса).
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
        // Обычные веса Jimenez — те же, что в bloom_down: без гашения firefly'ев.
        result  = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    u_dst[tid.xy] = float4(result, 1.0);
}
