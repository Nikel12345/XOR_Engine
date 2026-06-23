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
    float baseColor[4]     = { 1.0f, 1.0f, 1.0f, 1.0f };   // rgb-тинт (× albedo); a — резерв
    float emissive[3]      = { 0.0f, 0.0f, 0.0f };          // эмиссия (добавляется к освещению)
    float emissiveStrength = 1.0f;
    float metallic         = 0.0f;   // сила спекуляра (Blinn-Phong, пока только от directional)
    float roughness        = 1.0f;   // → shininess (1 = матовый, 0 = резкий блик)
};
// Раскладка cbuffer: baseColor[0..16] + emissive(16..28)+strength(28..32) + metallic(32..36)+
// roughness(36..40) → 48 (последний регистр добивается). На CPU те же смещения.
static_assert(sizeof(OpaqueMaterialParams) == 48,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");
