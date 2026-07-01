#ifndef MAIN_UNTEXTURED_SURFACE_HLSL
#define MAIN_UNTEXTURED_SURFACE_HLSL

// ПОЛЬЗОВАТЕЛЬСКАЯ часть ТЕКСТУРЕЛЕСС main-материала. Ни одной текстуры: цвет — фактор,
// нормаль — геометрическая (интерполированная из VS). Контракт/слоты — из пролога.
#include "main_pass/untextured/material_api.hlsl"

// Раскладка совпадает с OpaqueMaterialParams (C++): baseColor — это сам цвет (не тинт).
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 baseColor;
    float3 emissive;          // дублирует OpaqueMaterialParams (C++)
    float  emissiveStrength;
    float  metallic;
    float  roughness;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    SurfaceData s;
    s.baseColor = baseColor.rgb;
    float3 n    = normalize(input.v_worldNormal);   // нормаль-мапы нет → геометрическая
    s.normal    = isFrontFace ? n : -n;
    s.alpha     = baseColor.a * input.v_alpha;
    s.emission  = emissive * emissiveStrength;
    s.metallic  = metallic;
    s.roughness = roughness;
    s.ao        = 1.0;   // AO-карты нет → без затенения непрямого света
    return s;
}

// База пасса (свет/тени + main) — в конце; её storage-регистры взяты из макросов пролога (t1/t2).
#include "main_pass/main_pass.frag.hlsl"

#endif
