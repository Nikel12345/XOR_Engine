#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <SDL3/SDL_gpu.h>
#include "RenderCommandData.h"

struct BufferData;
struct PushConstantBinder;

namespace RenderSnap {

    struct ShadowCam {
        float   max_range = 0.0f;
        uint8_t is_ortho = 0;
    };

    struct LightCams {
        std::vector<ShadowCam> cams;
        // Число ИСТОЧНИКОВ, а не камер.
        uint32_t num_lights = 0;
    };

    struct TextureDraw {
        std::vector<UVL_Block> texture_uvl;
        VariantLayout variant_layout;
        const std::vector<uint8_t>* params = nullptr;
        uint32_t indirect_command_index = 0;
        uint32_t draw_count = 0;
    };

    struct AtlasGroup {
        std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
        std::vector<TextureDraw> draws;
    };

    struct ShaderGroup {
        std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
        PushInstructions push_instructions;
        std::vector<BufferData*> vertexBuffers;
        BufferData* indexBuffer = nullptr;
        std::vector<BufferData*> vertexStorageBuffers;
        std::vector<BufferData*> fragmentStorageBuffers;
        std::vector<AtlasGroup> atlases;
    };

    struct PassDrawList {
        std::vector<ShaderGroup> shaders;
        std::vector<SDL_GPUTextureSamplerBinding> global_texture_bindings;
        uint32_t first_instance = 0;
        uint32_t num_instances = 0;
        uint32_t num_commands = 0;
    };

    struct BatchLayout {
        std::vector<PassDrawList> passes;
        BufferData* indirectBuffer = nullptr;
    };
}
