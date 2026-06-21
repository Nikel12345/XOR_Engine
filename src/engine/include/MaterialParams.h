#pragma once
#include "MaterialData.h"   // MaterialParamsKind


struct alignas(16) TransparentMaterialParams
{
    static constexpr MaterialParamsKind kind = MaterialParamsKind::Transparent;
    float alpha = 1.0f;
};
static_assert(sizeof(TransparentMaterialParams) == 16,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");


struct alignas(16) OpaqueMaterialParams
{
    static constexpr MaterialParamsKind kind = MaterialParamsKind::Opaque;
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   // rgb-тинт (× albedo); a — резерв
    // float metallic         = 0.0f;   // нужен PBR-свет (+ опц. metallic-текстура)
    // float roughness        = 1.0f;
    // float emissive[3]      = { 0.0f, 0.0f, 0.0f };
    // float emissiveStrength = 1.0f;
};
static_assert(sizeof(OpaqueMaterialParams) == 16,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");
