#pragma once
#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include "ShaderTypes.h"
#include "Aliases.h"
#include "ResourceId.h"
#include "ResourceTags.h"

struct SpBinding {
    ShaderProgramId sp;

    // ВЛАДЕНИЕ РАЗДЕЛЁННОЕ: на блоб смотрит ещё и слепок рендера, живущий несколько кадров после
    // того, как материал ячейку снял или пережил загрузку сцены. Кто отпустит последним, тот и
    // освободит — материалу не нужно ни доживать до этого, ни вести кладбище снятых блобов.
    std::shared_ptr<std::vector<uint8_t>> params;

    // ИНВАРИАНТ: пустой params_type равносилен пустому блобу (держат Apply/ClearMaterialParams).
    std::string params_type;
};

struct Material {
    // Порядок ролей задаёт нумерацию ячеек состояний, поэтому контейнер обязан быть упорядоченным.
    std::map<TextureSlotRole, std::vector<TextureId>> textures;

    std::vector<SpBinding> shader_programs;

    ResourceTag tags = ResourceTag::None;

    SpBinding* FindBinding(ShaderProgramId sp_id) {
        for (SpBinding& b : shader_programs) if (b.sp == sp_id) return &b;
        return nullptr;
    }
    const SpBinding* FindBinding(ShaderProgramId sp_id) const {
        for (const SpBinding& b : shader_programs) if (b.sp == sp_id) return &b;
        return nullptr;
    }
};

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
