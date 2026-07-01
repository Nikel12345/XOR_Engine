#ifndef SURFACE_HLSL
#define SURFACE_HLSL

#include "main_pass/material_api.hlsl"

cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 baseColorFactor;   // rgb-тинт × albedo
    float3 emissive;          // дублирует OpaqueMaterialParams (C++)
    float  emissiveStrength;
    float  metallic;
    float  roughness;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    SurfaceData s;
    float4 alb  = SampleAlbedo(input.v_uv);
    s.baseColor = alb.rgb * baseColorFactor.rgb;
    s.normal    = SampleNormalWorld(input, isFrontFace);
    s.alpha     = alb.a * input.v_alpha;

    // ORM: R=AO, G=Roughness, B=Metallic. Текстура — множитель фактора (дефолт-белая ORM
    // даёт ao=1 и roughness/metallic = чистые факторы → поведение без карты не меняется).
    float3 orm  = SampleORM(input.v_uv);
    s.ao        = orm.r;
    s.roughness = roughness * orm.g;
    s.metallic  = metallic  * orm.b;

    // Эмиссия = карта × цвет-фактор × сила. Дефолт-белая карта → эмиссия = фактор×сила,
    // а дефолтный фактор (0) гасит свечение полностью — как было до текстуры.
    s.emission  = SampleEmissive(input.v_uv) * emissive * emissiveStrength;
    return s;
}

#include "main_pass/main_pass.frag.hlsl"

#endif
