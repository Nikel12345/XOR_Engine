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
    // Движковые render-программы (Lit/ShadowCaster/…) и их push-константы — дефолты движка
    // (Engine::InitDefaultShaders). Из сцены идут только СВОИ: фрактальные фоны.

    // Регистрация push-констант render-sp ПО ИМЕНИ в реестре ShaderManager. push_func — код-байндинг,
    // он НЕ сериализуется: загруженная из манифеста sp рождается без него. Реестр это и решает —
    // зовётся ОДИН РАЗ на инициализации, ДО первой LoadScene, а привязка к sp идёт сама.
    // Пуши фрактальных фонов ("Fractal"/"Mandelbrot") регистрируются ОБА, без разбора имени сцены:
    // сцена привозит свой sp — он и получит свою функцию, вторая просто ждёт (одна строка в логе).
    void RegisterShaderFuncs(EngineContext* ctx);

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
