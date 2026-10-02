#pragma once
#include "Aliases.h"

class ShaderManager;
class PassManager;
class BufferManager;
class TextureManager;
class CameraManager;
class ObjectManager;
class BatchBuilder;
class EngineContext;
class LightDataModule;
class CullingDataModule;

namespace DefaultShaderProgramSet
{
    // Зовётся ПЕРЕД созданием шейдеров: разбор маркеров //@push сверяется с реестром типов прямо
    // на компиляции, и тип, зарегистрированный позже, будет отмечен в логе как неизвестный.
    void SetDefaultPushes(EngineContext* ctx);

    // Движковый набор шейдеров: vs/fs/cs + render-программы штатных проходов (зовёт SetDefaultPushes
    // сам, первой строкой). Зовётся из конструктора движка, после DefaultResourceSet — нужны
    // готовыми пул геометрии, буферы и проходы.
    void SetDefaultShaders(EngineContext* ctx);

    void SetCullingPrograms(EngineContext* ctx, CullingDataModule* culling_module);
    void AddPassCulling(EngineContext* ctx, CullingDataModule* culling_module,
                        const RenderPassName& pass_name, BufferDataName camera_buffer, bool player_view);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetBloomPrograms(EngineContext* ctx);
    void SetAOPrograms(EngineContext* ctx);
    void SetFogProgram(EngineContext* ctx);
}
