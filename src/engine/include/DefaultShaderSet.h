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
    // push-константы — движковые дефолты (Engine::InitDefaultShaders). Здесь — вторая половина
    // того же набора: compute-программы. Они держат УКАЗАТЕЛИ на буферы/атласы и код-байндинги
    // (push/dispatch), поэтому не сериализуются и создаются кодом, а не манифестом сцены.
    // Зовёт их игра из MainInit: набор проходов у неё уже свой, а порядок вызова значим
    // (программы читают ординалы проходов, см. WARNINGS.md).

    // GPU-каллинг (culling_pib.comp): программа на проход с батчами, каждая пишет регион
    // своего прохода в out_pib/индиректе (раскладку штампует PassManager).
    // Проход создаёт engine (SetDefaultCullingPass); здесь программа + её push/dispatch.
    void SetCullingPibPrograms(EngineContext* ctx);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);

    // Программы bloom-пирамиды (prefilter/down/up/composite), привязка к BLOOM_PASS по имени.
    // Проход создаёт engine (DefaultRenderPassNamespace::SetDefaultBloomPass); сюда вынесены сами
    // программы и их push/dispatch — атласы берутся по имени, локальных зависимостей от прохода нет.
    void SetBloomPrograms(EngineContext* ctx);
}
