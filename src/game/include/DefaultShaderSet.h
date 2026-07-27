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
    // Render shader programs (sp/sp_shadow/sp_transparent/sp_untextured/sp_debug_collider) больше
    // НЕ создаются кодом — идут из сцены (shaders.json). Осталась только привязка их push-констант.

    // Пере-привязка push-констант к render-sp ПО ИМЕНИ. push_func — код-байндинг, он НЕ
    // сериализуется: после LoadScene загруженные sp (пересозданы из манифеста) остаются без него,
    // поэтому его вешают здесь, ПОСЛЕ загрузки сцены. Вызывать даже если sp созданы кодом —
    // единственная точка привязки push (в Set*-функциях его больше нет). Промах sp → пропуск.
    void BindDefaultPushFuncs(EngineContext* ctx);

    // Compute shader programs
    // GPU-каллинг (culling_pib.comp): пишет out_pib блоками по камерам (0 — игрок, 1..N — световые).
    // Проход создаёт engine (SetDefaultCullingPass); здесь программа + её push/dispatch.
    void SetCullingPibPrograms(EngineContext* ctx, LightDataModule* ldm);
    void SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm);

    // Программы bloom-пирамиды (prefilter/down/up/composite), привязка к BLOOM_PASS по имени.
    // Проход создаёт engine (DefaultRenderPassNamespace::SetDefaultBloomPass); сюда вынесены сами
    // программы и их push/dispatch — атласы берутся по имени, локальных зависимостей от прохода нет.
    void SetBloomPrograms(EngineContext* ctx);

    // UI-рендер-программа sp_ui (ui_vs/ui_fs), привязка к UI_PASS. Объявляет UI-буферы (transform/
    // outpib/instance у VS; bits/wordbase/index/text/glyphUVL у FS) — их usage бейкает эти буферы.
    // Слот Albedo = фон. Вызывать ДО первого BakePending (в MainInit).
    void SetUIProgram(EngineContext* ctx);
}