#pragma once

struct alignas(16) TransparentMaterialParams
{
    float alpha = 1.0f;
};
static_assert(sizeof(TransparentMaterialParams) == 16,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");


struct alignas(16) OpaqueMaterialParams
{
    float baseColor[4]     = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissive[3]      = { 0.0f, 0.0f, 0.0f };
    float emissiveStrength = 1.0f;
    float metallic         = 0.0f;
    float roughness        = 1.0f;
    float heightScale      = 0.0f;   // 0 = POM выключен
    float pomBias          = 0.0f;   // 1..3 — укрупнение рельефа; без мип-цепочки у normal-атласа молча ничего
};
static_assert(sizeof(OpaqueMaterialParams) == 48,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");


struct alignas(16) UIMaterialParams
{
    float bg_color[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float text_height   = 1.0f;   // доля высоты ректа под строку
    float text_anchor   = 0.0f;   // 0 = верх, 0.5 = центр, 1 = низ
    float _pad[2]       = { 0.0f, 0.0f };
};
static_assert(sizeof(UIMaterialParams) == 48,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");
