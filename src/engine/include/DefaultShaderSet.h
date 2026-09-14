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
    // Compute-половина движкового набора: сами шейдеры и render-программы создаёт
    // Engine::InitDefaultShaders, здесь — compute-программы и их код-байндинги. Проходы под них
    // тоже заводит движок, а зовёт эти функции игра из MainInit — ординал своего прохода каждая
    // программа снимает на создании, то есть проходы к этому моменту обязаны существовать.

    // Зовётся ПЕРЕД созданием шейдеров: разбор маркеров //@push сверяется с реестром типов прямо
    // на компиляции, и тип, зарегистрированный позже, будет отмечен в логе как неизвестный.
    void SetDefaultPushes(EngineContext* ctx);

    void SetCullingPibPrograms(EngineContext* ctx);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetBloomPrograms(EngineContext* ctx);
    void SetAOPrograms(EngineContext* ctx);
    void SetFogProgram(EngineContext* ctx);
}
