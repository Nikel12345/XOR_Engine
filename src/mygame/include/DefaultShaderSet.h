#pragma once
#include <string>

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
    // Render shader programs больше НЕ создаются кодом — идут из сцены (shaders.json).
    // Осталась только привязка их push-констант.

    // Пере-привязка push-констант к render-sp ПО ИМЕНИ. push_func — код-байндинг, он НЕ
    // сериализуется: после LoadScene загруженные sp (пересозданы из манифеста) остаются без него,
    // поэтому его вешают здесь, ПОСЛЕ загрузки сцены. Промах sp → пропуск. Пуши фрактальных
    // фонов ("Fractal"/"Mandelbrot" из shaders.json своих сцен) — под if'ом с именем сцены.
    void BindDefaultPushFuncs(EngineContext* ctx, const std::string& scene_name);

    // GPU-каллинг (culling_pib.comp): пишет out_pib блоками по камерам (0 — игрок, 1..N — световые).
    // Проход создаёт engine (SetDefaultCullingPass); здесь программа + её push/dispatch.
    void SetCullingPibPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);

    // Программы bloom-пирамиды (prefilter/down/up/composite), привязка к BLOOM_PASS по имени.
    // Проход создаёт engine (DefaultRenderPassNamespace::SetDefaultBloomPass); сюда вынесены сами
    // программы и их push/dispatch — атласы берутся по имени, локальных зависимостей от прохода нет.
    void SetBloomPrograms(EngineContext* ctx);
}
