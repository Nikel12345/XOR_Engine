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
        // Тот же блоб, что у материала, а не копия: правку слайдером в инспекторе видно сразу.
        // Ссылка ВЛАДЕЮЩАЯ, поэтому материал вправе умереть раньше слепка (см. SpBinding::params).
        std::shared_ptr<std::vector<uint8_t>> params;
        std::shared_ptr<const PushInstructions> push_instructions;
        uint32_t indirect_command_index = 0;
        uint32_t draw_count = 0;
    };

    struct GpuResourceGroup {
        std::vector<BufferData*> vertexStorageBuffers;
        std::vector<BufferData*> fragmentStorageBuffers;
        std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
        std::vector<TextureDraw> draws;
    };

    struct ShaderGroup {
        std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
        std::vector<BufferData*> vertexBuffers;
        BufferData* indexBuffer = nullptr;
        std::vector<GpuResourceGroup> resources;
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

    struct ComputeDispatch {
        std::shared_ptr<SDL_GPUComputePipeline> pipeline;
        PushInstructions push_instructions;
        std::function<void(DispatchSizeBinder&, const void*)> dispatch_func;
        std::vector<BufferData*> ro_storage_buffers;
        std::vector<BufferData*> rw_storage_buffers;
        std::vector<TextureAtlas*> ro_storage_textures;
        std::vector<ComputeRWStorageTextureRef> rw_storage_textures;
        std::vector<TextureAtlas*> texture_binding;
        uint32_t threadcount_x = 1;
        uint32_t threadcount_y = 1;
        uint32_t threadcount_z = 1;
    };

    struct ComputeLayout {
        std::vector<std::vector<ComputeDispatch>> passes;
    };
}
