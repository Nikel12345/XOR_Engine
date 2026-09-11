#pragma once
#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include "ShaderTypes.h"
#include "Aliases.h"

struct SpBinding {
    ShaderName sp;

    std::unique_ptr<std::vector<uint8_t>> params;

    // ИНВАРИАНТ: пустой params_type равносилен пустому блобу (держат Apply/ClearMaterialParams).
    std::string params_type;
};

struct Material {
    // Порядок ролей задаёт нумерацию ячеек состояний, поэтому контейнер обязан быть упорядоченным.
    std::map<TextureSlotRole, std::vector<TextureName>> textures;

    std::vector<SpBinding> shader_programs;

    // Освободить блоб снятой sp может только смерть материала: на его адрес ещё несколько кадров
    // смотрит слепок рендера.
    std::vector<std::unique_ptr<std::vector<uint8_t>>> retired_params;

    bool dont_save = false;

    SpBinding* FindBinding(const ShaderName& sp_name) {
        for (SpBinding& b : shader_programs) if (b.sp == sp_name) return &b;
        return nullptr;
    }
    const SpBinding* FindBinding(const ShaderName& sp_name) const {
        for (const SpBinding& b : shader_programs) if (b.sp == sp_name) return &b;
        return nullptr;
    }
};

// Ячейка секции состояний = индекс в этом массиве, другого её определения нет. Читают двое,
// BatchBuilder и TextureStateDataModule, и разъезд между ними молчит.
struct VariativeRoles {
    TextureSlotRole role[MAX_VARIATIVE_SLOTS];
    uint32_t        count = 0;
};

inline VariativeRoles CollectVariativeRoles(const Material& m) {
    VariativeRoles out{};
    for (const auto& [role, names] : m.textures) {
        if (names.size() <= 1) continue;
        if (out.count >= MAX_VARIATIVE_SLOTS) break;
        out.role[out.count++] = role;
    }
    return out;
}
