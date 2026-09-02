#ifndef SURFACE_HLSL
#define SURFACE_HLSL

#include "main_pass/material_api.hlsl"

cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 baseColorFactor;   // rgb-тинт × albedo
    float3 emissive;          // дублирует OpaqueMaterialParams (C++)
    float  emissiveStrength;
    float  metallic;
    float  roughness;
    float  heightScale;       // глубина POM (0 = выключен); альфа normal-карты = ВЫСОТА (depth = 1-A)
    float  pomBias;           // префильтр POM к крупным деталям (нужны мипы у normal-атласа)
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    // POM: смещаем mesh-UV по глубине (альфа normal-карты, depth=alpha) и view-вектору.
    // heightScale=0 → ApplyParallax возвращает исходный uv (no-op, дефолт для материалов без POM).
    float2 uv = ApplyParallax(input, heightScale, pomBias);

    SurfaceData s;
    float4 alb  = SampleAlbedo(uv);
    // Альфа-тест по маске выреза в АЛЬФЕ альбедо (у trim-листов она двоичная: 0 или 255).
    // Непрозрачный проход альфу не блендит, поэтому единственный способ ею воспользоваться —
    // выбросить фрагмент целиком; в отличие от блендинга это не требует сортировки и не мешает
    // записи глубины. Безусловен: текстуры без маски приходят с A=255 (SDL_ConvertSurface
    // доливает непрозрачную альфу источникам без альфа-канала) и тест проходят все.
    // Цена — аппаратный early-Z у программ с этим фрагментником выключается.
    clip(alb.a - 0.5);
    s.baseColor = alb.rgb * baseColorFactor.rgb;
    s.normal    = SampleNormalWorld(input, uv, isFrontFace);
    s.alpha     = alb.a * input.v_alpha;

    // ORM: R=AO, G=Roughness, B=Metallic. Текстура — множитель фактора (дефолт-белая ORM
    // даёт ao=1 и roughness/metallic = чистые факторы → поведение без карты не меняется).
    float3 orm  = SampleORM(uv);
    s.ao        = orm.r;
    s.roughness = roughness * orm.g;
    s.metallic  = metallic  * orm.b;

    // Эмиссия = карта × цвет-фактор × сила. Дефолт-белая карта → эмиссия = фактор×сила,
    // а дефолтный фактор (0) гасит свечение полностью — как было до текстуры.
    s.emission  = SampleEmissive(uv) * emissive * emissiveStrength;
    return s;
}

#include "main_pass/main_pass.frag.hlsl"

#endif
