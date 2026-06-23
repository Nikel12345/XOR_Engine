#ifndef TRANSPARENT_SURFACE_HLSL
#define TRANSPARENT_SURFACE_HLSL

// ПОЛЬЗОВАТЕЛЬСКАЯ часть прозрачного материала. Движковый контракт — из пролога; здесь
// только свои факторы + логика поверхности.
#include "transparent_pass/material_api.hlsl"

// Свои факторы. Дублирует TransparentMaterialParams (C++). Слот — из макроса пролога.
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float u_alpha;   // множитель прозрачности
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    SurfaceData s;
    float4 alb  = SampleAlbedo(input.v_uv);
    s.baseColor = alb.rgb;
    s.normal    = SampleNormalWorld(input, isFrontFace);
    s.alpha     = alb.a * u_alpha * input.v_alpha;   // текстура × per-material × per-instance
    return s;
}

// База пасса (свет + main) подключается В КОНЦЕ — это обычный фрагментный шейдер, штатный
// CreateFragmentShader, без движковой склейки.
#include "transparent_pass/transparent.frag.hlsl"

#endif
