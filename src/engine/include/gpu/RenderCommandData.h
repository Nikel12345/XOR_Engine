#pragma once
#include <bit>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <memory>
#include <SDL3/SDL_gpu.h>
#include "Aliases.h"
#include "ShaderTypes.h"
#include "TextureData.h"

struct BufferData;
struct TextureData;
struct TextureAtlas;

class PassManager;

// Строки трансформа нет: сущность transformless (её vs строит позицию сам) либо запись ещё не
// заполнена. Оба случая читаются одинаково.
inline constexpr uint32_t kPibNoRow = 0xFFFFFFFFu;

struct SubMeshDraw {
    uint32_t index_count = 0;
    uint32_t index_offset = 0;
    uint32_t vertex_offset = 0;
};

struct PibRecord {
    uint32_t entity = 0;
    uint32_t row = kPibNoRow;
};

// Сущности, которые в проходе рисуются одинаковым набором команд. Все листы группы делят её
// записи PIB: first_instance/num_instances у них общие.
struct DrawGroup {
    std::vector<PibRecord> records;
    uint32_t pib_first = 0;
    uint8_t  lod_count = 1;
    float    switches[3] = {};
};

struct ModelBatchData {
    DrawGroup*  group = nullptr;
    uint8_t     level = 0;
    uint32_t    firstInstance = 0;
    uint32_t    instanceCount = 0;
    SubMeshDraw submesh;
};

// Должно совпадать с material_api.hlsl: шейдер читает блок как uint4 и распаковывает
// unpackUnorm2x16, поэтому пары x/y обязаны лежать в одном слове.
struct alignas(16) UVL_Block {
    uint16_t uv_offset_x = 0;
    uint16_t uv_offset_y = 0;
    uint16_t uv_scale_x = 0;
    uint16_t uv_scale_y = 0;
    uint32_t layer = 0;
};
static_assert(std::endian::native == std::endian::little,
              "UVL_Block ложится в GPU-слова побайтно");
static_assert(sizeof(UVL_Block) == 4 * sizeof(uint32_t));

inline UVL_Block MakeUVL(const TextureData& placement) {
    return { placement.uv_offset_x, placement.uv_offset_y,
             placement.uv_scale_x,  placement.uv_scale_y, placement.layer };
}

// Должно совпадать с material_api.hlsl.
struct SlotWord {
    uint8_t  count = 0;
    uint8_t  cell  = 0;
    uint16_t base  = 0;
};
static_assert(std::endian::native == std::endian::little,
              "SlotWord ложится в GPU-слово побайтно");
static_assert(sizeof(SlotWord) == sizeof(uint32_t) && alignof(SlotWord) <= alignof(uint32_t));

// Должно совпадать с material_api.hlsl.
struct VariantLayout {
    SlotWord slot[MAX_SLOTS] = {};
    uint32_t material_index = 0;
};

struct TextureBatchData {
    std::unordered_map<BatchKeys::ModelBatchKey, ModelBatchData> model_batches;
	std::vector<UVL_Block> texture_uvl;
    VariantLayout variant_layout;
    uint32_t indirect_command_index = 0;
    std::shared_ptr<std::vector<uint8_t>> params;
};

struct MatSpLayout {
    std::vector<UVL_Block>                    uvl;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
    SlotWord                slot[MAX_SLOTS] = {};
    BatchKeys::MatSpKey     res_key = 0;
    BatchKeys::AtlasBatchKey atlas_key = 0;
    bool                    bindable = false;
    bool                    variative = false;
};

struct AtlasBatchData {
    std::unordered_map<BatchKeys::TextureBatchKey, TextureBatchData> texture_batches;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
};

struct ShaderBatchData {
    PushInstructions push_instructions;
    std::unordered_map<BatchKeys::AtlasBatchKey, AtlasBatchData> atlases_batches;
	std::vector<BufferData*> vertexBuffers;
	BufferData* indexBuffer = nullptr;
    std::vector<BufferData*> vertexStorageBuffers;
    std::vector<BufferData*> fragmentStorageBuffers;
    std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
};

// Потолок SDL (MAX_COLOR_TARGET_BINDINGS в его исходниках).
inline constexpr uint32_t MAX_COLOR_TARGETS = 8;

struct ColorTarget {
    SDL_GPUColorTargetInfo info{}; 
    SDL_GPUTextureFormat   format = SDL_GPU_TEXTUREFORMAT_INVALID;
    TextureAtlas*          atlas = nullptr;
};

struct RenderPassTexturesInfo {
    // append-only: каждый вызов добавляет НОВЫЙ color target, то есть ещё один выход фрагментника
    void CreateColorTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_FColor color, SDL_GPUTextureFormat format);
    void CreateDepthTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_GPUTextureFormat format);

    void SetColorTexture(TextureAtlas* atlas, uint32_t index = 0);
    void SetDepthTexture(TextureAtlas* atlas);
    void ResolveTargets();

    void SetColorTargetInfoLayer(uint32_t layer, uint32_t index = 0) { color_targets[index].info.layer_or_depth_plane = layer; };
    uint32_t CollectColorTargetInfos(SDL_GPUColorTargetInfo* out, uint32_t capacity) const;

    std::vector<ColorTarget> color_targets;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
    TextureAtlas*      depth_atlas = nullptr;
private:
};

struct RenderPassStep {
    RenderPassTexturesInfo renderPassTexsData;
    std::unordered_map<BatchKeys::ShaderBatchKey, ShaderBatchData> shader_batches;
    std::unordered_map<BatchKeys::GroupKey, DrawGroup> draw_groups;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function;

    std::vector<TextureAtlas*> global_texture_bindings;
    void SetGlobalTextures(std::vector<TextureAtlas*> atlases);

    // ПОТОКИ: пишет render-поток (тело прохода) и он же UI — UI рисуется внутри RenderFunc.
    std::vector<uint8_t> state;
    std::string          state_type;
    template<class T> T* State() {
        return state.size() >= sizeof(T) ? reinterpret_cast<T*>(state.data()) : nullptr;
    }
    std::string debug_name;
    int pass_index = -1;
    uint32_t ordinal = 0;
};

struct BlitPassStep {
    TextureAtlas* src = nullptr;
    TextureAtlas* dst = nullptr;
    uint32_t src_mip = 0;
    uint32_t src_layer = 0;
    SDL_GPUFilter filter = SDL_GPU_FILTER_NEAREST;
    SDL_GPULoadOp load_op = SDL_GPU_LOADOP_DONT_CARE;
    std::string debug_name;
    int pass_index = -1;
};

struct ComputeRWStorageTextureRef {
    TextureAtlas* atlas = nullptr;
    uint32_t mip_level = 0;
    uint32_t layer = 0;
};

struct ComputePassStep {
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function;
    std::vector<uint8_t> state;
    std::string          state_type;
    template<class T> T* State() {
        return state.size() >= sizeof(T) ? reinterpret_cast<T*>(state.data()) : nullptr;
    }
    std::string debug_name;
    int pass_index = -1;
    uint32_t ordinal = UINT32_MAX;
};
