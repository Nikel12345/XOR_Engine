#pragma once
#include <unordered_set>
#include "ShaderData.h"

struct TextureHandle;

struct Material {
    std::unordered_map<TextureSlotRole, TextureHandle*> textures;
    std::unordered_set<ShaderProgram*> shader_programs;
    // Прозрачность материала (1 = непрозрачный). Едет на GPU в .w UVL-cbuffer альбедо;
    // opaque-шейдеры её игнорируют, так что поле безвредно для всех материалов.
    float alpha = 1.0f;
};
