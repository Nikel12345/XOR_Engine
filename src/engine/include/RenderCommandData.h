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

class PassManager;

// BatchKeys НЕ вливается в глобал директивой — ключи квалифицированы явно.

struct ModelBatchData {
    std::vector<uint32_t> pib_sub_buffer;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    SubMeshData* submesh = nullptr;
};


// UVL батча — то, что пушится fragment-uniform'ом материала (в шейдере это uint4, см.
// material_api.hlsl). Отдельно от TextureData: та — CPU-запись о размещении в атласе и на GPU
// не едет, поэтому её поля (число слоёв и прочая бухгалтерия упаковщика) сюда не просачиваются.
// У глиф-буфера своя раскладка со своим смыслом четвёртого слова — см. GlyphUVL в FontManager.h.
struct alignas(16) UVL_Block {
    uint32_t uv_packed_offset = 0;
    uint32_t uv_packed_scale = 0;
    uint32_t layer = 0;
};

inline UVL_Block MakeUVL(const TextureData& td) {
    return { td.uv_packed_offset, td.uv_packed_scale, td.layer };
}

struct TextureBatchData {
    std::unordered_map<BatchKeys::ModelBatchKey, ModelBatchData> model_batches;
	// UVL хранится ЗНАЧЕНИЯМИ (не указателями): непрерывный блок → прямой пуш в Execute
	// без per-draw сбора разбросанных указателей. Инвариант: значения копируются при
	// сборке батча, поэтому смена UVL ЖИВОЙ текстуры (репак/компактизация атласа) ОБЯЗАНА
	// триггерить BuildRenderBatches. Добавление/удаление текстур этого не нарушают: чужие
	// UVL не двигаются, а батч удаляемой текстуры и так пересобирается.
	std::vector<UVL_Block> texture_uvl;
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
	// Индексный буфер пула, которому принадлежат стримы vs (у одного пула — один).
	BufferData* indexBuffer = nullptr;
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
    // Таргеты задаются АТЛАСАМИ, не сырыми SDL_GPUTexture*: GPU-текстуры на момент объявления
    // прохода ещё не существует (её создаёт бейк), а ресайз её подменяет. Резолв — на исполнении
    // (ResolveTargets), поэтому ни бейк, ни ресайз не требуют переназначать таргеты по проходам.
    void SetColorTexture(TextureAtlas* atlas, uint32_t index = 0);
    void SetDepthTexture(TextureAtlas* atlas);
    // Атласы → colorTargetInfos[i].texture / depthTargetInfo.texture.
    void ResolveTargets();

    void SetColorTargetInfoLayer(uint32_t layer, uint32_t index = 0) { colorTargetInfos[index].layer_or_depth_plane = layer; };
    // Параллельные массивы: colorTargetInfos[i] — рантайм-привязка (texture/clear/layer),
    // color_formats[i] — формат таргета i для построения пайплайна (PipeManager),
    // color_atlases[i] — ИСТОЧНИК текстуры таргета i (резолв в ResolveTargets).
    // Размер = число MRT-выходов.
    std::vector<SDL_GPUColorTargetInfo> colorTargetInfos;
    std::vector<SDL_GPUTextureFormat>   color_formats;
    std::vector<TextureAtlas*>          color_atlases;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
    // Источник depth: атлас (или nullptr — проход без depth). depth-таргет — обычный TextureAtlas.
    TextureAtlas*      depth_atlas = nullptr;
};

struct RenderPassStep {
    RenderPassTexturesInfo renderPassTexsData;
    std::unordered_map<BatchKeys::ShaderBatchKey, ShaderBatchData> shader_batches;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function;
    // Глобальные сэмплеры прохода (слоты 0..N-1 фрагментного шейдера, ДО батчевых): тень, env-куб.
    // Держим АТЛАСЫ, а не готовые SDL_GPUTextureSamplerBinding: указатель на атлас стабилен, а
    // GPU-текстуру внутри могут пересоздать — копия биндинга, снятая на setup, протухнет (и под
    // отложенной инициализацией GPU-ресурсов её на setup ещё попросту нет). Резолв в SDL-биндинги —
    // на сборке батча, в слепок (RenderSnap::PassDrawList::global_texture_bindings). Ровно тот же
    // приём, что у compute (ComputeShaderBatchData::texture_binding) и у BlitPassStep.
    // Роль однозначна — SDL_BindGPUFragmentSamplers, т.е. для каждого атласа это SAMPLER.
    // ЗАПОЛНЯТЬ ТОЛЬКО ЧЕРЕЗ SetGlobalTextures — он же собирает флаг в атласы.
    std::vector<TextureAtlas*> global_texture_bindings;
    // Единственная точка записи global_texture_bindings: ставит атласы и копит им SAMPLER.
    void SetGlobalTextures(std::vector<TextureAtlas*> atlases);
    // ТОЛЬКО для UI (дропдаун прохода у sp) и логов. В логике не использовать: sp хранит имя
    // прохода сама (render_pass_name) — см. правило про debug_name в CLAUDE.md.

    // ── Состояние прохода ──
    // То, что раньше было ЛОКАЛЬНОЙ структурой в теле прохода: тело пишет свои поля сюда и
    // отдаёт указатель на блоб вниз (push_data_raw), а push-функции программ собирают из него
    // свои cbuffer'ы. Разница с локальной переменной ровно одна — хранилище ПЕРЕЖИВАЕТ кадр,
    // поэтому его можно показать редактору: поля, которые тело не переписывает, и есть настройки
    // прохода. Схема — по имени в ParamsSpecRegistry::Passes() (ставит SetPassState).
    // ПОТОКИ: пишет render-поток (тело прохода) и он же UI (UI рисуется внутри RenderFunc).
    std::vector<uint8_t> state;
    std::string          state_type;
    // Типизированный доступ тела прохода к своему же блобу. nullptr — если состояние не заводили
    // или его размер меньше T: это рассинхрон объявления и использования, а не штатный случай.
    template<class T> T* State() {
        return state.size() >= sizeof(T) ? reinterpret_cast<T*>(state.data()) : nullptr;
    }
    std::string debug_name;
    int pass_index = -1;
    // Порядковый номер в ordered_passes (ставит FillRenderPasses) — индекс прохода в
    // RenderSnap::BatchLayout::passes. Стабилен после старта.
    uint32_t ordinal = 0;
};


// Блит-проход: ОДИН блит src→dst, целиком ДАННЫЕ (без render_function). Функтора здесь нет
// намеренно: усложни его до лямбды — и вывод usage-флагов снова станет невозможен (внутрь
// std::function не заглянуть), а блит ЕДИНСТВЕННАЯ операция вне шейдерных биндов, которой
// флаги реально нужны: SDL требует SAMPLER у src и COLOR_TARGET у dst (проверено зондом,
// sandbox/BlitUsageProbe.cpp; в докстрингах SDL этого нет). Нужно несколько блитов — заводи
// несколько проходов, порядок задаёт pass_index.
//
// src/dst — TextureAtlas* (НЕ SDL_GPUTexture*): указатель на атлас стабилен, а текстуру внутри
// подменяют (ResizeSceneHDRTargets пересоздаёт HDR-таргеты; свопчейн-атлас PassManager'а меняет
// её каждый кадр). Поэтому шаг переживает и ресайз, и смену свопчейна без перепривязки.
struct BlitPassStep {
    TextureAtlas* src = nullptr;
    TextureAtlas* dst = nullptr;   // может быть свопчейн-атлас (PassManager::GetSwapchainAtlas)
    uint32_t src_mip = 0;
    uint32_t src_layer = 0;
    SDL_GPUFilter filter = SDL_GPU_FILTER_NEAREST;
    SDL_GPULoadOp load_op = SDL_GPU_LOADOP_DONT_CARE;
    std::string debug_name;   // ТОЛЬКО для UI/логов (см. CLAUDE.md)
    int pass_index = -1;
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
    uint32_t threadcount_x = 1;
    uint32_t threadcount_y = 1;
    uint32_t threadcount_z = 1;
    SDL_GPUComputePipeline* pipeline = nullptr;
};

struct ComputePassStep {
    std::vector<ComputeShaderBatchData> shader_batches;
    std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function;
    // ТОЛЬКО для UI/логов (см. CLAUDE.md): csp хранит имя прохода сама (compute_pass_name).
    // NB про резолв по этому имени: пространство имён у пассов и препассов ОБЩЕЕ —
    // CreateComputePass/CreateComputePrepass отказывают, если имя занято в соседнем реестре,
    // поэтому «сначала пасс, иначе препасс» однозначно.
    // ── Состояние прохода ──
    // То, что раньше было ЛОКАЛЬНОЙ структурой в теле прохода: тело пишет свои поля сюда и
    // отдаёт указатель на блоб вниз (push_data_raw), а push-функции программ собирают из него
    // свои cbuffer'ы. Разница с локальной переменной ровно одна — хранилище ПЕРЕЖИВАЕТ кадр,
    // поэтому его можно показать редактору: поля, которые тело не переписывает, и есть настройки
    // прохода. Схема — по имени в ParamsSpecRegistry::Passes() (ставит SetPassState).
    // ПОТОКИ: пишет render-поток (тело прохода) и он же UI (UI рисуется внутри RenderFunc).
    std::vector<uint8_t> state;
    std::string          state_type;
    // Типизированный доступ тела прохода к своему же блобу. nullptr — если состояние не заводили
    // или его размер меньше T: это рассинхрон объявления и использования, а не штатный случай.
    template<class T> T* State() {
        return state.size() >= sizeof(T) ? reinterpret_cast<T*>(state.data()) : nullptr;
    }
    std::string debug_name;
    int pass_index = -1;
};

