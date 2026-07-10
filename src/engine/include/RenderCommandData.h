#pragma once
#include <vector>
#include <unordered_map>
#include <SDL3/SDL_gpu.h>
#include "Aliases.h"
#include "MaterialData.h"
#include "TextureData.h"

struct SubMeshData;
struct BufferData;
struct TextureData;
struct TextureAtlas;
struct SharedDepthTarget;

class PassManager;

// BatchKeys НЕ вливается в глобал директивой — ключи квалифицированы явно.

struct ModelBatchData {
    std::vector<uint32_t> pib_sub_buffer;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    SubMeshData* submesh = nullptr;
};

struct TextureBatchData {
    std::unordered_map<BatchKeys::ModelBatchKey, ModelBatchData> model_batches;
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
    std::unordered_map<BatchKeys::TextureBatchKey, TextureBatchData> texture_batches;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
};

struct ShaderBatchData {
    std::function<void(const PushConstantBinder&, const void*)> push_func = {};
    std::unordered_map<BatchKeys::AtlasBatchKey, AtlasBatchData> atlases_batches;
	std::vector<BufferData*> vertexBuffers;
    std::vector<BufferData*> vertexStorageBuffers;
    std::vector<BufferData*> fragmentStorageBuffers;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    uint32_t frag_uniform_count = 0;   // число fragment uniform-буферов шейдера (гейт пуша params)
};

struct RenderPassTexturesInfo {
    // append-only: КАЖДЫЙ вызов добавляет новый color target (MRT). Один вызов → один таргет,
    // два вызова → два выхода фрагментного шейдера (location 0,1) за один проход геометрии.
    void CreateColorTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_FColor color, SDL_GPUTextureFormat format);
    void CreateDepthTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_GPUTextureFormat format);
    void SetColorTexture(SDL_GPUTexture* tex, uint32_t index = 0);
    void SetDepthTexture(SDL_GPUTexture* tex);
    // Привязка к разделяемому depth-таргету: фактический texture резолвится лениво в
    // RenderPassStandardBody, поэтому ресайз таргета не требует переназначения по проходам.
    void SetDepthTexture(SharedDepthTarget* dt);

    void SetColorTargetInfoLayer(uint32_t layer, uint32_t index = 0) { colorTargetInfos[index].layer_or_depth_plane = layer; };
    // Параллельные массивы: colorTargetInfos[i] — рантайм-привязка (texture/clear/layer),
    // color_formats[i] — формат таргета i для построения пайплайна (PipeManager). Размер = число MRT-выходов.
    std::vector<SDL_GPUColorTargetInfo> colorTargetInfos;
    std::vector<SDL_GPUTextureFormat>   color_formats;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
    SharedDepthTarget* shared_depth = nullptr;   // != nullptr → depth берётся отсюда (лениво)
};

struct RenderPassStep {
    RenderPassTexturesInfo renderPassTexsData;
    std::unordered_map<BatchKeys::ShaderBatchKey, ShaderBatchData> shader_batches;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function;
    std::vector<SDL_GPUTextureSamplerBinding> global_texture_bindings;
    std::string debug_name;   // = ключ в render_steps; нужен UI для дропдауна прохода у sp
    int pass_index = -1;
    // Порядковый номер в ordered_passes (ставит FillRenderPasses) — индекс прохода в
    // RenderSnap::BatchLayout::passes. Стабилен после старта.
    uint32_t ordinal = 0;
};


struct ComputeRWStorageTextureRef {
    TextureAtlas* atlas = nullptr;
    uint32_t mip_level = 0;
    uint32_t layer = 0;
};

struct ComputeShaderBatchData {
    std::function<void(const PushConstantBinder&, const void*)> push_func = {};
    std::function<void(DispatchSizeBinder&, const void*)> dispatch_func = {};
    std::vector<BufferData*> ro_storage_buffers; // set=0, SDL_BindGPUComputeStorageBuffers
    std::vector<BufferData*> rw_storage_buffers; // set=1, SDL_BeginGPUComputePass
    std::vector<TextureAtlas*> ro_storage_textures;
    std::vector<ComputeRWStorageTextureRef> rw_storage_textures;
    std::vector<TextureAtlas*> texture_binding;
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

