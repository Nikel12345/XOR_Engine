#pragma once
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>
#include "ShaderData.h"

struct TextureHandle;

// Тег раскладки params — ТОЛЬКО для тулинга (UI/инспектор). Рендер params не интерпретирует
// (непрозрачный блоб), но UI по тегу выбирает рукописный разбор полей. None → неизвестно
// (UI крутит сырые float). Выставляется автоматически из T::kind в SetMaterialParams.
enum class MaterialParamsKind : uint32_t { None = 0, Opaque, Transparent };

struct Material {
    // weak_ptr, а не сырой указатель: текстуру могут удалить независимо от материала
    // (TextureManager::DeleteTextureHandle). weak_ptr::lock() → nullptr для удалённой → BatchBuilder
    // подставит dummy. Так «висячий указатель» невозможен by design, без ручной инвалидации.
    std::unordered_map<TextureSlotRole, std::weak_ptr<TextureHandle>> textures;
    std::unordered_set<ShaderProgram*> shader_programs;

    // Непрозрачный блоб per-material факторов (alpha, baseColorFactor, metallic, ...). Layout
    // задаёт автор шейдера (его cbuffer); движок видит только байты и пушит их как есть.
    // Адрес &params стабилен (Material живёт под unique_ptr в MaterialManager) → им же
    // ключуется texture-батч: мутация байт на месте НЕ меняет ключ (твики в рантайме без
    // перестройки дерева), а разные материалы дают разные адреса (не схлопываются).
    std::vector<uint8_t> params;
    MaterialParamsKind   params_kind = MaterialParamsKind::None;   // тег для UI-разбора (см. выше)
};
