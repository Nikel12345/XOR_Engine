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
    // Render-программы (Lit/LitColor/LitTransparent/ShadowCaster/Wireframe/Skybox/UI) и их
    // push-константы — движковые дефолты (Engine::InitDefaultShaders). Игре остаются только
    // compute-программы: они держат УКАЗАТЕЛИ на буферы/атласы, поэтому создаются кодом.

    // GPU-каллинг (culling_pib.comp): программа на проход с батчами, каждая пишет регион
    // своего прохода в out_pib/индиректе (раскладку считает AskRegions).
    // Проход создаёт engine (SetDefaultCullingPass); здесь программа + её push/dispatch.
    void SetCullingPibPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);

    // Программы bloom-пирамиды (prefilter/down/up/composite), привязка к BLOOM_PASS по имени.
    // Проход создаёт engine (DefaultRenderPassNamespace::SetDefaultBloomPass); сюда вынесены сами
    // программы и их push/dispatch — атласы берутся по имени, локальных зависимостей от прохода нет.
    void SetBloomPrograms(EngineContext* ctx);

}