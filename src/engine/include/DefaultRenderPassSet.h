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
    inline constexpr const char* DEBUG_PASS = "_DefaultDebugRenderPass";
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
        Uint32 cameraIndex;
        float  max_range;
    };
    void SetDefaultShadowPCFRenderPass(EngineContext* ctx);
    void SetDefaultShadowVSMRenderPass(EngineContext* ctx);

    struct ShadowBlurUniform { uint32_t layerIndex; };
    struct DummyDispatchData {};

    void SetDefaultShadowBlurPass(EngineContext* ctx);
	void SetDefaultMainRenderPass(EngineContext* ctx);
    void SetDefaultMainRenderPass(EngineContext* ctx, SDL_GPUDevice* dev, SDL_Window* win);

    // Цвет фрагмента дебаг-коллайдеров (push-констант, fragment slot 0).
    struct alignas(16) DebugColliderPushData { float color[4] = { 0.0f, 1.0f, 0.2f, 1.0f }; };
    // Пасс поверх свопчейна: рисует батчи дебаг-шейдера (рамки коллайдеров) после MAIN_PASS.
    // Требует уже инициализированного MAIN_PASS (берёт его depth-текстуру пассивно).
    void SetDebugColliderPass(EngineContext* ctx);
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