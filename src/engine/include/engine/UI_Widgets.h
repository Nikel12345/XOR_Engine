#pragma once
#include <string>
#include "imgui.h"

namespace ui {

enum class AssetIcon { Texture, Model, Material, Shader, Compute, Vsd, Fsd, Csd, Generic };

struct TilePreview {
    ImTextureID tex = 0;
    ImVec2 uv0{ 0, 0 }, uv1{ 1, 1 };
    ImVec4 tint{ 1, 1, 1, 1 };
};

bool AssetTile(const char* name, bool selected, float size, AssetIcon icon = AssetIcon::Generic,
               const TilePreview* preview = nullptr);

bool PlusTile(float size);

bool DangerButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

}
