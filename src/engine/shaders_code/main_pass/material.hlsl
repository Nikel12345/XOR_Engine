#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "main_pass/math.hlsl"

// Сэмплирование атласной текстуры.
// textureData: x = packed offset (unorm2x16), y = packed scale (unorm2x16), z = layer
float4 sampleAtlas(
    Texture2DArray atlas,
    SamplerState   samp,
    uint4          textureData,
    float2         uv)
{
    float2 offset = unpackUnorm2x16(textureData.x);
    float2 scale  = unpackUnorm2x16(textureData.y);
    uint   layer  = textureData.z;
    return atlas.Sample(samp, float3(uv * scale + offset, float(layer)));
}

// Чтение нормали из normal map (атлас) и перевод в world space через TBN.
float3 computeNormal(
    Texture2DArray normalMap,
    SamplerState   normalSampler,
    uint4          normalTextureData,
    float2         uv,
    float3         worldTangent,
    float3         worldBitangent,
    float3         worldNormal)
{
    float3x3 TBN = float3x3(
        worldTangent,
        worldBitangent,
        normalize(worldNormal)
    );
    // Нормаль-маппинг. Освещение нелинейно по нормали, а фильтрация сглаживает лишь ВЕКТОР нормали
    // → высокочастотная нормаль недосэмпливается и её шейдинг мерцает при движении камеры (тёмное
    // дрожание, на тёплом дереве читается как цветовой сдвиг). Полностью убрать субпиксельное
    // мерцание при сильном рельефе можно только TAA; здесь — компромисс двумя ручками:
    //   NORMAL_STRENGTH — сила рельефа. Больше = выразительнее, но заметнее мерцает.
    //   NORMAL_MIP_BIAS — префильтр (блюр нормали, +к авто-LOD). Больше = меньше мерцания, мягче
    //                     рельеф. Работает ТОЛЬКО с aniso=OFF у DEFAULT_SAMPLER (иначе резкая ось
    //                     футпринта не даёт мипу префильтровать) — см. TextureSamplerPresets.h.
    const float NORMAL_STRENGTH = 0.7;
    const float NORMAL_MIP_BIAS = 1.0;
    float2 noff = unpackUnorm2x16(normalTextureData.x);
    float2 nsc  = unpackUnorm2x16(normalTextureData.y);
    float3 n = normalMap.SampleBias(normalSampler, float3(uv * nsc + noff, float(normalTextureData.z)), NORMAL_MIP_BIAS).rgb * 2.0 - 1.0;
    n.xy *= NORMAL_STRENGTH;
    return normalize(mul(n, TBN));
}

// ── Parallax Occlusion Mapping ───────────────────────────────────────────────
// Данные рельефа живут в АЛЬФЕ normal-атласа (RGB=нормаль, A=HEIGHT) → отдельного сэмплера/атласа/
// слота нет, POM переиспользует u_normal. Конвенция: A = ВЫСОТА (яркое=выше — стандарт *_h карт),
// марч идёт по depth = 1 - A вниз от верха поверхности (depth 0) до пересечения. heightScale =
// глубина рельефа (0 = выкл). Ассеты с инвертированной альфой (cavity-карты, яркое=глубже, как
// старая кора wood_normal_h) надо инвертировать на стороне АССЕТА, не в шейдере.
// NB: ранее «работавший» depth=A на кваде был двойной инверсией (левосторонняя развёртка квада
// переворачивала ось v марча) — после исправления развёртки корректен именно 1-A.

// Сэмпл глубины из альфы атласа. Внутри динамического цикла авто-LOD запрещён → используем
// SampleGrad с ЯВНЫМИ градиентами (посчитаны до цикла): корректная трилинейная фильтрация против
// высокочастотного алиасинга. pomBias (>0) дополнительно поднимает LOD → префильтр рельефа к
// крупным формам (кора → пряди вместо «ряби»). ВАЖНО: bias работает ТОЛЬКО если у normal-атласа
// есть мип-цепочка (NormalAtlas(..., FullMipLevels)); без мипов bias — no-op. Per-material.
float sampleDepthAtlas(
    Texture2DArray atlas,
    SamplerState   samp,
    uint4          textureData,
    float2         uv,
    float2         ddx_uv,
    float2         ddy_uv,
    float          pomBias)
{
    // Клампим tile-UV к [0..1]: марч смещает UV за пределы тайла, а атласная раскладка
    // (uv*scale+offset) не wrap'ает — без клампа сэмпл ушёл бы в соседний тайл атласа (bleed).
    uv = saturate(uv);
    float2 offset = unpackUnorm2x16(textureData.x);
    float2 scale  = unpackUnorm2x16(textureData.y);
    uint   layer  = textureData.z;
    // Координата сэмпла = uv*scale+offset → её градиент = d(uv)*scale (offset — константа).
    // exp2(pomBias) масштабирует градиент → поднимает LOD (префильтр крупного рельефа).
    float  g = exp2(pomBias);
    // A = height (яркое=выше) → depth = 1 - A: верх поверхности (A=1) даёт depth 0 (без смещения).
    return 1.0 - atlas.SampleGrad(samp, float3(uv * scale + offset, float(layer)),
                                  ddx_uv * scale * g, ddy_uv * scale * g).a;
}

// Рей-марч depth-поля в tangent space: возвращает СМЕЩЁННЫЙ mesh-UV (в [0..1] тайла — атласную
// раскладку накладывает каждый сэмпл материала отдельно). Vt — направление НА камеру в tangent
// space (нормализовано). heightScale — глубина рельефа в UV-единицах (0 = POM выключен).
//
// OFFSET LIMITING: сдвиг = Vt.xy * heightScale (БЕЗ деления на Vt.z). Деление на Vt.z геометрически
// «точнее», но на скользящих углах (Vt.z→0) взрывает сдвиг до пол-текстуры → размазывание/искажение.
// Offset-limiting ограничивает |сдвиг| ≤ heightScale на любом угле — стабильно, без grazing-артефактов.
float2 parallaxOcclusionUV(
    Texture2DArray depthAtlas,
    SamplerState   samp,
    uint4          textureData,
    float2         uv,
    float3         Vt,
    float          heightScale,
    float2         ddx_uv,
    float2         ddy_uv,
    float          pomBias)
{
    if (heightScale <= 0.0) return uv;

    const float minLayers   = 16.0;
    const float maxLayers   = 48.0;
    const int   binarySteps = 6;    // бинарное уточнение после линейного марча
    float numLayers  = lerp(maxLayers, minLayers, saturate(Vt.z));
    float layerDepth = 1.0 / numLayers;

    float2 P       = Vt.xy * heightScale;   // offset-limited: |сдвиг| ≤ heightScale
    float2 deltaUV = P * layerDepth;

    float2 curUV         = uv;
    float  curLayerDepth = 0.0;
    float  curDepth      = sampleDepthAtlas(depthAtlas, samp, textureData, curUV, ddx_uv, ddy_uv, pomBias);

    // ЛИНЕЙНЫЙ поиск: шагаем вниз, пока луч не окажется под depth-полем (грубый интервал).
    [loop]
    for (int i = 0; i < (int)maxLayers; ++i)
    {
        if (curLayerDepth >= curDepth) break;
        curUV         -= deltaUV;
        curDepth       = sampleDepthAtlas(depthAtlas, samp, textureData, curUV, ddx_uv, ddy_uv, pomBias);
        curLayerDepth += layerDepth;
    }

    // Плоскость / нет проникновения (depth≈0 на верхе) → цикл не шагнул: без смещения (иначе
    // бинарный поиск ниже уехал бы с места). curLayerDepth>0 ⇔ был хотя бы один шаг.
    if (curLayerDepth <= 0.0) return uv;

    // БИНАРНОЕ уточнение (Relief Mapping): сходимся к точной точке пересечения в найденном
    // интервале. Это убирает ступенчатость линейного марча — «дублирование/шлейф» поднятых
    // деталей и грубое размазывание вблизи (там аппаратный шаг слишком крупный на фоне рельефа).
    float2 dUV    = deltaUV;
    float  dLayer = layerDepth;
    [unroll]
    for (int j = 0; j < binarySteps; ++j)
    {
        dUV    *= 0.5;
        dLayer *= 0.5;
        curDepth = sampleDepthAtlas(depthAtlas, samp, textureData, curUV, ddx_uv, ddy_uv, pomBias);
        if (curLayerDepth < curDepth) { curUV -= dUV; curLayerDepth += dLayer; }   // над полем → глубже
        else                          { curUV += dUV; curLayerDepth -= dLayer; }   // под полем → назад
    }
    return curUV;
}

#endif