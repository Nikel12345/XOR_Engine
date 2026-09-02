#ifndef SSAO_BLUR_HLSLI
#define SSAO_BLUR_HLSLI

// Общее тело билатерального блюра AO. Включается ПОСЛЕ объявления биндингов (как sparse_rank.hlsli
// у материалов): горизонтальный и вертикальный проходы отличаются только тем, какая текстура —
// источник, какая — приёмник, и осью; сам фильтр один.
//
// Билатеральный, а не гауссов: обычное размытие тянет затенение через силуэт, и объект получает
// нимб от фона за ним. Вес тапа гасится по разрыву ГЛУБИНЫ — соседи с другой поверхности в
// свёртку не входят. Порог берём в долях радиуса AO: дальше него затенение уже не связано.
//
// Ядро — БОКС из 4 тапов с РАВНЫМИ весами, и это не «попроще», а обязательное условие: ssao
// поворачивает своё ядро по тайлу 4x4, и только бокс ровно 4x4 усредняет все 16 поворотов в
// равных долях, сокращая рисунок точно. Гаусс (1-4-6-4-1) даёт центру больший вес — часть
// рисунка переживает свёртку и видна как полосы.
//
// Требует объявленными до включения: u_src/u_srcS (AO-источник), u_depth/u_depthS (глубина
// полного разрешения), u_dst (r32f приёмник), cbuffer AOParams (нужен radius).

void SsaoBlurAxis(uint2 pix, float2 axis)
{
    uint w, h; u_dst.GetDimensions(w, h);
    if (pix.x >= w || pix.y >= h) return;

    const float2 texel = 1.0 / float2(w, h);
    const float2 uv    = (float2(pix) + 0.5) * texel;
    const float2 step  = axis * texel;

    const float dc = u_depth.SampleLevel(u_depthS, uv, 0);
    if (dc >= 1.0) { u_dst[pix] = 1.0; return; }   // небо: блюрить нечего, и глубины для весов нет

    // View-глубина центра — в НЕЙ сравниваем соседей: разрыв в единицах буфера глубины нелинеен
    // и у горизонта схлопывается, порог поплыл бы с дистанцией.
    const float A = Camera[0].proj[2][2];
    const float B = Camera[0].proj[2][3];
    const float zc = B / (A + dc);                 // = -ViewZ, положительная
    const float dzMax = max(radius * 0.5, 1e-4);

    float sum = 0.0;
    float wsum = 0.0;
    [unroll]
    for (int i = -1; i <= 2; ++i)   // 4 тапа: период тайла ssao
    {
        const float2 suv = uv + step * float(i);
        const float  sd  = u_depth.SampleLevel(u_depthS, suv, 0);
        if (sd >= 1.0) continue;

        const float zs = B / (A + sd);
        const float wt = saturate(1.0 - abs(zs - zc) / dzMax);   // вес ТОЛЬКО билатеральный

        sum  += u_src.SampleLevel(u_srcS, suv, 0) * wt;
        wsum += wt;
    }

    u_dst[pix] = (wsum > 1e-5) ? (sum / wsum) : u_src.SampleLevel(u_srcS, uv, 0);
}

#endif
