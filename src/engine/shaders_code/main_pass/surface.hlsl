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
    s.emission  = emissive * emissiveStrength;
    s.metallic  = metallic;
    s.roughness = roughness;
    return s;
}

#include "main_pass/main_pass.frag.hlsl"

#endif
