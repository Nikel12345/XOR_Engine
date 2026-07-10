#pragma once
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "ShaderTypes.h"   // TextureSlotRole (лёгкая половина; тяжёлый ShaderData.h не нужен)
#include "Aliases.h"

// Тег раскладки params — ТОЛЬКО для тулинга (UI/инспектор). Рендер params не интерпретирует
// (непрозрачный блоб), но UI по тегу выбирает рукописный разбор полей. None → неизвестно
// (UI крутит сырые float). Выставляется автоматически из T::kind в SetMaterialParams.
enum class MaterialParamsKind : uint32_t { None = 0, Opaque, Transparent };

struct Material {
    // Пользовательский ресурс ссылается на другой ТОЛЬКО по имени (правило редактора): не
    // указатель/weak_ptr, а имя, резолвится лениво в BatchBuilder на сборке батчей (TextureManager
    // по имени → TextureHandle). Промах (текстура удалена/переименована/ещё не создана) → nullptr →
    // dummy в батче. В отличие от weak_ptr, имя ПЕРЕЖИВАЕТ delete+recreate под тем же именем: ссылка
    // перепривязывается на новый хэндл при следующей пересборке, без ручной инвалидации.
    std::unordered_map<TextureSlotRole, TextureName> textures;
    std::vector<ShaderName> shader_programs;   // имена sp (материал может жить в нескольких пассах)

    // Непрозрачный блоб per-material факторов (alpha, baseColorFactor, metallic, ...). Layout
    // задаёт автор шейдера (его cbuffer); движок видит только байты и пушит их как есть.
    // Адрес &params стабилен (Material живёт под unique_ptr в MaterialManager) → им же
    // ключуется texture-батч: мутация байт на месте НЕ меняет ключ (твики в рантайме без
    // перестройки дерева), а разные материалы дают разные адреса (не схлопываются).
    std::vector<uint8_t> params;
    MaterialParamsKind   params_kind = MaterialParamsKind::None;   // тег для UI-разбора (см. выше)
    // Не писать в materials.json при SaveScene (кодовая инфраструктура — напр. debug_collider,
    // используемый только генератором debug-рамок). UI/пересоздание → false («тронул = сохраняемый»).
    bool dont_save = false;
};
