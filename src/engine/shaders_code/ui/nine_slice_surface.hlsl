#ifndef UI_NINE_SLICE_SURFACE_HLSL
#define UI_NINE_SLICE_SURFACE_HLSL

#include "ui/nine_slice_api.hlsl"

// Дублирует UINineSliceMaterialParams (MaterialParams.h).
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 tint;
};

float4 getColor(PSInput input)
{
    return SampleNineSlice(input) * tint;
}

#include "ui/nine_slice.frag.hlsl"

#endif
