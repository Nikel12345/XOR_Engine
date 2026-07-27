#pragma once
#include "TextureData.h"
class EngineContext;
class LightDataModule;
namespace DefaultRenderPassNamespace
{
    inline constexpr const char* DEPTH_PASS = "_DefaultDepthRenderPass";
    inline constexpr const char* MAIN_PASS = "_DefaultMainRenderPass";
    inline constexpr const char* TRANSPARENT_PASS = "_DefaultTransparentRenderPass";
    inline constexpr const char* DEBUG_PASS = "_DefaultDebugRenderPass";
    inline constexpr const char* PRESENT_PASS = "_DefaultPresentPass";
    inline constexpr const char* BLOOM_PASS = "_DefaultBloomPass";
    inline constexpr const char* UI_PASS = "_DefaultUIPass";

    // Число уровней bloom-пирамиды (bloom_0 = ½ окна, каждый следующий ещё вдвое меньше).
    inline constexpr uint32_t BLOOM_LEVELS = 4;
    inline constexpr const char* SHADOW_PASS = "_DefaultShadowRenderPass";
    inline constexpr const char* CULLING_PASS = "_DefaultCullingPass";
    inline constexpr const char* SHADOW_BLUR_PASS = "_DefaultBlurPass";
    inline const std::string SHADOW_DEPTH_FLAT_ARRAY = "shadow_depth_flat_array";


    struct alignas(16) ShadowPushData
    {
        Uint32 camera_index;
        float  max_range;
        // 1 — directional (ortho): в карту пишется линейная осевая глубина -viewZ/far.
        // 0 — spot/sphere (perspective): евклидова дистанция length(viewPos)/far.
        Uint32 is_ortho;
        // Размер одного камерного блока out_pib (= число PIB-записей). Вершиннику
        // shadow_pass нужен для адреса блока: (1 + camera_index) * num_instances.
        Uint32 num_instances;
    };
    // ldm — источник слепка теневых камер (AskShadowCameras(slot)): проход итерирует его
    // таблицу, а не ECS, поэтому camera_index совпадает с LIGHT_CAMERA_BUFFER слота.
    void SetDefaultShadowPCFRenderPass(EngineContext* ctx, LightDataModule* ldm);
    void SetDefaultShadowVSMRenderPass(EngineContext* ctx, LightDataModule* ldm);

    struct ShadowBlurUniform { uint32_t layerIndex; };
    struct DummyDispatchData {};
    void SetDefaultShadowBlurPass(EngineContext* ctx);

    // PassSystem: общие ресурсы дефолтного набора проходов (разделяемый depth-таргет, его формат).
    // Должна вызываться ПЕРЕД Set*Pass, которые их потребляют (main/transparent/debug).
    void _SetDefaultCommonResources(EngineContext* ctx, uint32_t width, uint32_t height);

    void SetDefaultMainRenderPass(EngineContext* ctx);
    void SetDefaultMainRenderPass(EngineContext* ctx, SDL_GPUDevice* dev, SDL_Window* win);

    struct alignas(16) DebugColliderPushData { float color[4] = { 0.0f, 1.0f, 0.2f, 1.0f }; };
    void SetDebugColliderPass(EngineContext* ctx);

    void SetTransparentPass(EngineContext* ctx);

    // UI-оверлей: рендер UI-энтити (NDC-квады) в scene_hdr ПОСЛЕ bloom, ДО present (не блумится).
    // Своя глубина (main_depth с CLEAR — z-пространство UI отдельное), __TextAtlas как глобалка.
    void SetUIPass(EngineContext* ctx);

    // Финальный проход: blit HDR-сцены (scene_hdr) в свопчейн с конвертацией формата.
    // Регистрируется последним (приоритет 30). Тонмаппинг появится на этапе bloom-composite.
    void SetPresentPass(EngineContext* ctx);

    // Пересоздаёт HDR-таргеты набора (scene_hdr/scene_emission + уровни bloom) под новый размер
    // окна и переинъектит их во все проходы (MAIN/TRANSPARENT/DEBUG). Обязана вызываться из
    // обработчика ресайза ВМЕСТЕ с ресайзом depth — иначе размеры color/depth аттачментов разойдутся.
    void ResizeSceneHDRTargets(EngineContext* ctx, uint32_t width, uint32_t height);

    // Push-константы bloom-программ (один layout на все: down/up/composite). Раскладка совпадает с
    // cbuffer BloomParams в шейдерах comp/bloom_*.comp.hlsl.
    struct alignas(16) BloomParams {
        uint32_t useKaris  = 0;     // 1 только на первом downsample (гасит fireflies)
        float    intensity = 0.0f;  // доля подмешивания bloom в composite (~0.05)
        float    threshold = 0.0f;  // prefilter: порог яркости, ниже которого сцена не блумит
        float    knee      = 0.0f;  // prefilter: ширина мягкого колена вокруг порога
        uint32_t srcLod    = 0;     // down/up: mip-уровень источника в единой bloom-пирамиде
    };

    // Создаёт compute-проход bloom: пирамида downsample→upsample по эмиссии + composite/tonemap в
    // scene_hdr. Должна вызываться ПОСЛЕ main/transparent/debug (читает их результат) и до present.
    void SetDefaultBloomPass(EngineContext* ctx);

    // GPU-каллинг с компактацией (culling_pib.comp = scatter). Одна программа на группу камер:
    // раскладка = CullParams шейдера. Никакого is_shadow — группу задаёт push + камерный буфер.
    struct alignas(16) CullingPibUniform {
        uint32_t range_start;      // первая PIB-запись программы (диапазон прохода)
        uint32_t range_count;      // сколько записей (= размер диспатча)
        uint32_t block_base;       // первый камерный блок (0 игрок, 1 свет, …)
        uint32_t num_blocks;       // число камер группы
        uint32_t block_stride;     // N — всего PIB-записей (страйд блока out_pib)
        uint32_t total_commands;   // TC — команд на камеру (страйд блока индиректа)
    };
    // culling_clear.comp: обнуляет num_instances всех (камера,команда) перед scatter.
    struct alignas(16) CullingClearUniform {
        uint32_t total_slots;      // (1+L) * total_commands
    };
    void SetDefaultCullingPass(EngineContext* ctx);

    inline const std::string SHADOW_MOMENTS_ARRAY = "shadow_moments_array";
    inline const std::string SHADOW_MOMENTS_BLUR_TEMP = "shadow_moments_single_temp";
}