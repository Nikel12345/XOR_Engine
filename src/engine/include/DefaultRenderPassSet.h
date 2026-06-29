#pragma once
#include "TextureData.h"
class EngineContext;
class TransformDataModule;
class LightDataModule;
class IndirectDataModule;
namespace DefaultRenderPassNamespace
{
    inline constexpr const char* DEPTH_PASS = "_DefaultDepthRenderPass";
    inline constexpr const char* MAIN_PASS = "_DefaultMainRenderPass";
    inline constexpr const char* TRANSPARENT_PASS = "_DefaultTransparentRenderPass";
    inline constexpr const char* DEBUG_PASS = "_DefaultDebugRenderPass";
    inline constexpr const char* PRESENT_PASS = "_DefaultPresentPass";
    inline constexpr const char* BLOOM_PASS = "_DefaultBloomPass";

    // Число уровней bloom-пирамиды (bloom_0 = ½ окна, каждый следующий ещё вдвое меньше).
    inline constexpr uint32_t BLOOM_LEVELS = 6;
    inline constexpr const char* SHADOW_PASS = "_DefaultShadowRenderPass";
    inline constexpr const char* CULLING_PREPASS = "_DefaultCullingComputePass";
    inline constexpr const char* CULLING_ZEROS_PREPASS = "_DefaultCullingZerosComputePass";
    inline constexpr const char* CULLING_OFFSET_PREPASS = "_DefaultCullingOffsetComputePass";
    inline constexpr const char* CULLING_OUT_INDIRECT_PREPASS = "_DefaultCullingOutIndirectComputePass";
    inline constexpr const char* CULLING_WRITE_PASS = "_DefaultCullingWritePass";
    inline constexpr const char* SHADOW_BLUR_PASS = "_DefaultBlurPass";
    inline const std::string SHADOW_DEPTH_FLAT_ARRAY = "shadow_depth_flat_array";


    struct alignas(16) ShadowPushData
    {
        Uint32 camera_index;
        float  max_range;
        // 1 — directional (ortho): в карту пишется линейная осевая глубина -viewZ/far.
        // 0 — spot/sphere (perspective): евклидова дистанция length(viewPos)/far.
        Uint32 is_ortho;
    };
    void SetDefaultShadowPCFRenderPass(EngineContext* ctx);
    void SetDefaultShadowVSMRenderPass(EngineContext* ctx);

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
        float    _pad0     = 0.0f;
        float    _pad1     = 0.0f;
    };

    // Создаёт compute-проход bloom: пирамида downsample→upsample по эмиссии + composite/tonemap в
    // scene_hdr. Должна вызываться ПОСЛЕ main/transparent/debug (читает их результат) и до present.
    void SetDefaultBloomPass(EngineContext* ctx);

    void SetDefaultCullingComputeZerosPass(EngineContext* ctx);

    struct ComputeCullingCountUniform {
        uint32_t num_instances;
        uint32_t num_commands;
        uint32_t num_cameras;
        uint32_t cmd_offset;
    };
    void SetDefaultCullingComputeCountPass(EngineContext* ctx, TransformDataModule* tdm, LightDataModule* ldm, IndirectDataModule* idm);
    void SetDefaultCullingOffstPass(EngineContext* ctx);

    struct ComputeCullingOutIndirectUniform {
        uint32_t num_commands;
        uint32_t num_cameras;
        uint32_t cmd_offset;
    };
    void SetDefaultCullingOutIndirectPass(EngineContext* ctx);
    void SetDefaultCullingOutTransformPass(EngineContext* ctx, TransformDataModule* tdm, LightDataModule* ldm, IndirectDataModule* idm);

    inline const std::string SHADOW_MOMENTS_ARRAY = "shadow_moments_array";
    inline const std::string SHADOW_MOMENTS_BLUR_TEMP = "shadow_moments_single_temp";
}