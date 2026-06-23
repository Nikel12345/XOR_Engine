#pragma once

class ShaderManager;
class PassManager;
class BufferManager;
class TextureManager;
class CameraManager;
class ObjectManager;
class BatchBuilder;
class EngineContext;
class LightDataModule;

namespace DefaultShaderProgramSet
{
    // Render shader programs
    void SetMainShaderProgram(EngineContext* ctx);
    void SetDefaultShadowShaderProgram(EngineContext* ctx);
    void SetTransparentShaderProgram(EngineContext* ctx);
    // Текстурелесс main-материал (цвет из фактора, без текстур): свой surface + свой sp (required_slots {}).
    void SetUntexturedShaderProgram(EngineContext* ctx);

    // Голый шейдер дебаг-рамок коллайдеров (без текстур), привязан к DEBUG_PASS.
    void SetDebugColliderProgram(EngineContext* ctx);

    // Compute shader programs
    void SetCullingZerosPrograms(EngineContext* ctx);
    void SetCullingCountPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetCullingOffsetPrograms(EngineContext* ctx);
    void SetCullingOutIndirectPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetCullingWritePrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);
}