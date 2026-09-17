#pragma once
#include <string>
#include "ComponentStorage.h"
#include "ShaderTypes.h"

namespace ui {

enum class SelKind { None, Entity, Camera, Material, Texture, Model, Shader, Compute,
                     Vsd, Fsd, Csd,
                     Pass,
                     UINode };

struct Selection {
    SelKind     kind    = SelKind::None;
    Entity      entity  = static_cast<Entity>(-1);
    int         index   = -1;
    std::string name;
    uint32_t    ui_node = 0xFFFFFFFFu;
};

extern Selection g_sel;
extern bool      g_show_internal;

constexpr float kPanelBgAlpha = 0.0f;

inline const char* RoleName(TextureSlotRole r)
{
    switch (r) {
    case TextureSlotRole::Albedo:   return "Albedo";
    case TextureSlotRole::Normal:   return "Normal";
    case TextureSlotRole::ORM:      return "ORM";
    case TextureSlotRole::Emissive: return "Emissive";
    case TextureSlotRole::Custom0:  return "Custom0";
    case TextureSlotRole::Custom1:  return "Custom1";
    case TextureSlotRole::Custom2:  return "Custom2";
    case TextureSlotRole::Custom3:  return "Custom3";
    case TextureSlotRole::Custom4:  return "Custom4";
    case TextureSlotRole::Custom5:  return "Custom5";
    case TextureSlotRole::Custom6:  return "Custom6";
    case TextureSlotRole::Custom7:  return "Custom7";
    default:                        return "Role";
    }
}

}
