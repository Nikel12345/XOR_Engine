#pragma once
#include <vector>
#include <unordered_map>
#include <SDL3/SDL_gpu.h>
#include "Aliases.h"
#include "MaterialData.h"
#include "TextureData.h"   // TextureData по значению в TextureBatchData::texture_uvl

struct SubMeshData;
struct BufferData;
struct TextureData;
struct TextureAtlas;
struct SharedDepthTarget;

class PassManager;

using namespace BatchKeys;

struct ModelBatchData {
    std::vector<uint32_t> pib_sub_buffer;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    SubMeshData* submesh = nullptr;
};

struct TextureBatchData {
    std::unordered_map<ModelBatchKey, ModelBatchData> model_batches;
	// UVL хранится ЗНАЧЕНИЯМИ (не указателями): непрерывный блок → прямой пуш в Execute
	// без per-draw сбора разбросанных указателей. Инвариант: значения копируются при
	// сборке батча, поэтому смена UVL ЖИВОЙ текстуры (репак/компактизация атласа) ОБЯЗАНА
	// триггерить BuildRenderBatches. Добавление/удаление текстур этого не нарушают: чужие
	// UVL не двигаются, а батч удаляемой текстуры и так пересобирается.
	std::vector<TextureData> texture_uvl;
    uint32_t indirect_command_index = 0;
    const std::vector<uint8_t>* params = nullptr;   // → &Material::params (невладеющий; адрес стабилен; alpha и пр. факторы внутри)
};

struct AtlasBatchData {
    std::unordered_map<TextureBatchKey, TextureBatchData> texture_batches;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
};

struct ShaderBatchData {
    std::function<void(const PushConstantBinder&, const void*)> push_func = {};
    std::unordered_map<AtlasBatchKey, AtlasBatchData> atlases_batches;
	std::vector<BufferData*> vertexBuffers;
    std::vector<BufferData*> vertexStorageBuffers;
    std::vector<BufferData*> fragmentStorageBuffers;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    uint32_t frag_uniform_count = 0;   // число fragment uniform-буферов шейдера (гейт пуша params)
};

struct RenderPassTexturesInfo {
    void CreateColorTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_FColor color, SDL_GPUTextureFormat format, Uint32 numColorTargets = 1);
    void CreateDepthTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_GPUTextureFormat format);
    void SetColorTexture(SDL_GPUTexture* tex);
    void SetDepthTexture(SDL_GPUTexture* tex);
    // Привязка к разделяемому depth-таргету: фактический texture резолвится лениво в
    // RenderPassStandardBody, поэтому ресайз таргета не требует переназначения по проходам.
    void SetDepthTexture(SharedDepthTarget* dt);

    void SetColorTargetInfoLayer(uint32_t layer) { colorTargetInfo.layer_or_depth_plane = layer; };
    SDL_GPUColorTargetInfo colorTargetInfo{};
    Uint32 numColorTargets = 0;
    SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
    SharedDepthTarget* shared_depth = nullptr;   // != nullptr → depth берётся отсюда (лениво)
};

struct RenderPassStep {
    RenderPassTexturesInfo renderPassTexsData;
    std::unordered_map<ShaderBatchKey, ShaderBatchData> shader_batches;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function;
    std::vector<SDL_GPUTextureSamplerBinding> global_texture_bindings;
    int pass_index = -1;
};

struct ComputeShaderBatchData {
    std::function<void(const PushConstantBinder&, const void*)> push_func = {};
    std::function<void(DispatchSizeBinder&, const void*)> dispatch_func = {};
    std::vector<BufferData*> ro_storage_buffers; // set=0, SDL_BindGPUComputeStorageBuffers
    std::vector<BufferData*> rw_storage_buffers; // set=1, SDL_BeginGPUComputePass
    std::vector<SDL_GPUTexture*> ro_storage_textures;
    std::vector<SDL_GPUStorageTextureReadWriteBinding> rw_storage_textures;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
    std::string debug_name;
    uint32_t threadcount_x = 1;
    uint32_t threadcount_y = 1;
    uint32_t threadcount_z = 1;
    SDL_GPUComputePipeline* pipeline = nullptr;
};

struct ComputePassStep {
    std::vector<ComputeShaderBatchData> shader_batches;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function;
    int pass_index = -1;
};

