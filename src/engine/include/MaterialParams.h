#pragma once
// Встроенные раскладки MaterialBlock. Схемы полей (имена в файле, диапазоны, подписи) —
// не здесь, а в RegisterBuiltinMaterialParamsSpecs (MaterialParamsSpec.cpp): структура знает
// только раскладку и ДЕФОЛТЫ (member-инициализаторы ниже — единственный их источник).


struct alignas(16) TransparentMaterialParams
{
    float alpha = 1.0f;
};
static_assert(sizeof(TransparentMaterialParams) == 16,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");


struct alignas(16) OpaqueMaterialParams
{
    float baseColor[4]     = { 1.0f, 1.0f, 1.0f, 1.0f };   // rgb-тинт (× albedo); a — резерв
    float emissive[3]      = { 0.0f, 0.0f, 0.0f };          // эмиссия (добавляется к освещению)
    float emissiveStrength = 1.0f;
    float metallic         = 0.0f;   // сила спекуляра (Blinn-Phong, пока только от directional)
    float roughness        = 1.0f;   // → shininess (1 = матовый, 0 = резкий блик)
    float heightScale      = 0.0f;   // глубина POM (parallax); 0 = ВЫКЛючатель POM. Альфа normal = ВЫСОТА (depth=1-A)
    // Префильтр рельефа POM в лог2-мипах (НЕ выключатель!): 0 = полная детализация, 1..3 = рабочий
    // диапазон укрупнения (глушит высокочастотный шум карты — «шпили»), <0.5 визуально ≈ ничего.
    // Требует мип-цепочку у normal-атласа (FullMipLevels), иначе silent no-op.
    float pomBias          = 0.0f;
};
// Раскладка cbuffer: baseColor[0..16] + emissive(16..28)+strength(28..32) + metallic(32..36)+
// roughness(36..40) + heightScale(40..44) + pomBias(44..48) → 48. heightScale/pomBias заняли
// бывший padding, поэтому размер не изменился. На CPU те же смещения.
static_assert(sizeof(OpaqueMaterialParams) == 48,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");


// UI-элемент: два цвета. bg_color — тинт фона (× albedo), text_color — цвет глифов (× покрытие
// из __TextAtlas). Оба редактируются в инспекторе и мутабельны на лету (hover/анимация через
// material->params). Один тип на все UI-элементы (у безтекстовых text_color просто не задействован).
struct alignas(16) UIMaterialParams
{
    float bg_color[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };   // тинт фона (× albedo-текстура)
    float text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   // цвет текста (× coverage глифа)
    // Вертикальная посадка строки внутри ректа. Растеризация УЖЕ держит базовую линию (SDL_ttf
    // отдаёт глиф в поверхности высотой line_height с готовой базой) — поэтому двигаем/масштабируем
    // полосу текста ЦЕЛИКОМ, база остаётся встроенной в пиксели атласа. text_height — доля высоты
    // ректа под строку (1 = во всю высоту, как было). text_anchor — 0=верх, 0.5=центр, 1=низ.
    // Дефолты (1, 0) дают прежнее поведение, поэтому старые материалы не меняются.
    float text_height   = 1.0f;
    float text_anchor   = 0.0f;
    float _pad[2]       = { 0.0f, 0.0f };   // добивка до float4 (cbuffer text_params)
};
static_assert(sizeof(UIMaterialParams) == 48,
              "MaterialBlock должен быть кратен 16 байтам (cbuffer-выравнивание)");
