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
    // Зовётся ПЕРЕД созданием шейдеров: разбор маркеров //@push сверяется с реестром типов прямо
    // на компиляции, и тип, зарегистрированный позже, будет отмечен в логе как неизвестный.
    void SetDefaultPushes(EngineContext* ctx);

    // Движковый набор шейдеров: vs/fs/cs + render-программы штатных проходов (зовёт SetDefaultPushes
    // сам, первой строкой). Зовётся из конструктора движка, после DefaultResourceSet — нужны
    // готовыми пул геометрии, буферы и проходы.
    void SetDefaultShaders(EngineContext* ctx);

    // Compute-программы: они держат УКАЗАТЕЛИ на буферы и атласы, поэтому не сериализуются.
    // Зовёт их игра из MainInit — ординал своего прохода каждая программа снимает на создании,
    // то есть проходы к этому моменту обязаны существовать.

    void SetCullingPibPrograms(EngineContext* ctx);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetBloomPrograms(EngineContext* ctx);
    void SetAOPrograms(EngineContext* ctx);
    void SetFogProgram(EngineContext* ctx);
}
