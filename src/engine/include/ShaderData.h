#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <functional>
#include <memory>
#include <SDL3/SDL_gpu.h>
#include "ShaderTypes.h"
#include "Aliases.h"
#include "ResourceId.h"
#include "ResourceTags.h"


struct BufferData;
struct RenderPassStep;
struct ComputePassStep;
struct TextureAtlas;

namespace ShaderBase {
    struct ShaderData
    {
        std::shared_ptr<SDL_GPUShader> shader;
        size_t shader_size = 0;
        Uint8* shader_code = nullptr;
        Uint32 num_uniform_buffers = 0;
    };
}

struct VertexShaderData {
    ShaderBase::ShaderData shader_data;
    std::vector<SDL_GPUVertexAttribute> attributes;
    std::vector<SDL_GPUVertexBufferDescription> vbs;
    std::string source_path;
    std::vector<ShaderBase::VertexBufferBinding> bindings;
    std::vector<BufferDataName> vertex_buffer_names;
    std::string    pool_name;
    BufferDataName index_buffer = nullptr;
    std::vector<ShaderDefine> defines;
    std::vector<std::string> push_kinds;
    ResourceTag tags = ResourceTag::None;
};

struct FragmentShaderData {
    ShaderBase::ShaderData shader_data;
    std::string source_path;
    std::vector<ShaderDefine> defines;
    std::vector<std::string> push_kinds;
    ResourceTag tags = ResourceTag::None;
};

struct ComputeShaderData {
	Uint8* spv_code = nullptr;
	size_t spv_size = 0;
    std::string source_path;
    std::vector<ShaderDefine> defines;
    Uint32 threadcount_x = 1;
    Uint32 threadcount_y = 1;
    Uint32 threadcount_z = 1;
    Uint32 num_samplers = 0;
    Uint32 num_readonly_storage_textures = 0;
    Uint32 num_readonly_storage_buffers = 0;
    Uint32 num_readwrite_storage_textures = 0;
    Uint32 num_readwrite_storage_buffers = 0;
    Uint32 num_uniform_buffers = 0;
    std::vector<std::string> push_kinds;
    ResourceTag tags = ResourceTag::None;
};

struct ShaderProgram {
    VertexShaderId vs_id;
    std::vector<BufferDataName> vertex_shader_buffer_names;

    FragmentShaderId fs_id;
    std::vector<BufferDataName> fragment_shader_buffer_names;

    std::vector<TextureSlotRole> required_slots;

	ShaderProgramDescription spd;
    // Ссылку держат ещё и слепки раскладки, поэтому пайплайн переживает удаление своей sp.
    std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
    RenderPassName render_pass_name;

    std::string debug_name;
    ResourceTag tags = ResourceTag::None;
};


struct ComputeShaderProgram {
    ComputeShaderId cs_id;
    std::vector<BufferDataName> rw_storage_buffer_names;
    std::vector<BufferDataName> ro_storage_buffer_names;
    std::vector<ComputeRWTextureBinding> rw_storage_textures;
    std::vector<AtlasId> ro_storage_texture_ids;
    std::vector<AtlasId> texture_sampler_ids;

    ComputePassName compute_pass_name;
    std::shared_ptr<SDL_GPUComputePipeline> pipeline;
    ResourceTag tags = ResourceTag::None;
    std::string debug_name;
};

struct ComputeProgramSlot {
    std::string name;
    std::unique_ptr<ComputeShaderProgram> program;
};
