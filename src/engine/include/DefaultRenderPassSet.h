#pragma once
#include "TextureData.h"
#include "RenderSnapshot.h"   // RenderSnap::Regions — возвращается по значению из AskRegions
class EngineContext;
class LightDataModule;
class BatchBuilder;
class PassManager;
namespace DefaultRenderPassNamespace
{
    // Группы камер дефолтного набора проходов. Номер группы — это и порядок её региона в
    // индиректе/out_pib (RenderSnap::BuildRegions), поэтому у игрока он 0: база его региона
    // всегда 0, и его проходы рисуют со смещением 0.
    inline constexpr uint32_t CAMERA_GROUP_PLAYER = 0;
    inline constexpr uint32_t CAMERA_GROUP_LIGHT = 1;

    // ЕДИНСТВЕННОЕ объявление связи «проход — камеры»: теневой проход рисуют световые камеры,
    // остальные — камера игрока. Живёт здесь, у набора, который эти камеры и заводит; ни сам
    // проход, ни ядро о камерах не знают. Считается ОДИН раз после FillRenderPasses (список
    // проходов дальше не меняется) и отдаётся BatchBuilder'у, который по этим прогонам
    // нумерует команды и кладёт их в раскладку.
    std::array<RenderSnap::GroupSpan, RenderSnap::kMaxCameraGroups> BuildCameraGroupSpans(PassManager* pm);

    // Регионы out-буферов для слота. ЕДИНСТВЕННЫЙ способ их получить: размеры буферов,
    // пуши каллинга и смещения дроу обязаны считать их одинаково, иначе адреса разъедутся
    // молча. Прогоны берутся из слепка раскладки, число камер — из слепка того же слота.
    RenderSnap::Regions AskRegions(BatchBuilder* bb, LightDataModule* ldm, uint8_t slot);
    inline constexpr const char* DEPTH_PASS = "_DefaultDepthRenderPass";
    inline constexpr const char* MAIN_PASS = "_DefaultMainRenderPass";
    inline constexpr const char* TRANSPARENT_PASS = "_DefaultTransparentRenderPass";
    inline constexpr const char* DEBUG_PASS = "_DefaultDebugRenderPass";
    inline constexpr const char* PRESENT_PASS = "_DefaultPresentPass";
    inline constexpr const char* BLOOM_PASS = "_DefaultBloomPass";
    inline constexpr const char* UI_PASS = "_DefaultUIPass";

    // Число уровней bloom-пирамиды (bloom_0 = ½ окна, каждый следующий ещё вдвое меньше).
    inline constexpr uint32_t BLOOM_LEVELS = 4;
    inline constexpr const char* SHADOW_PASS = "_DefaultShadowRenderPass";
    inline constexpr const char* CULLING_PASS = "_DefaultCullingPass";
    inline constexpr const char* SHADOW_BLUR_PASS = "_DefaultBlurPass";
    inline const std::string SHADOW_DEPTH_FLAT_ARRAY = "shadow_depth_flat_array";


    struct alignas(16) ShadowPushData
    {
        Uint32 camera_index;
        float  max_range;
        // 1 — directional (ortho): в карту пишется линейная осевая глубина -viewZ/far.
        // 0 — spot/sphere (perspective): евклидова дистанция length(viewPos)/far.
        Uint32 is_ortho;
    };
    // ldm — источник слепка теневых камер (AskShadowCameras(slot)): проход итерирует его
    // таблицу, а не ECS, поэтому camera_index совпадает с LIGHT_CAMERA_BUFFER слота.
    void SetDefaultShadowPCFRenderPass(EngineContext* ctx, LightDataModule* ldm);
    void SetDefaultShadowVSMRenderPass(EngineContext* ctx, LightDataModule* ldm);

    struct ShadowBlurUniform { uint32_t layerIndex; };
    struct DummyDispatchData {};
    void SetDefaultShadowBlurPass(EngineContext* ctx);

    // PassSystem: общие ресурсы дефолтного набора проходов (разделяемый depth-таргет, его формат).
    // Должна вызываться ПЕРЕД Set*Pass, которые их потребляют (main/transparent/debug).
    void _SetDefaultCommonResources(EngineContext* ctx, uint32_t width, uint32_t height);

    void SetDefaultMainRenderPass(EngineContext* ctx);
    void SetDefaultMainRenderPass(EngineContext* ctx, SDL_GPUDevice* dev, SDL_Window* win);

    // Состояние DEBUG_PASS: цвет рамок коллайдеров. Тело прохода его не трогает — это чистая
    // настройка, поэтому у прохода есть схема (имя ниже) и он редактируется.
    struct alignas(16) DebugColliderPushData { float color[4] = { 0.0f, 1.0f, 0.2f, 1.0f }; };
    inline const std::string DEBUG_COLLIDER_STATE = "DebugCollider";
    void SetDebugColliderPass(EngineContext* ctx);

    void SetTransparentPass(EngineContext* ctx);

    // UI-оверлей: рендер UI-энтити (NDC-квады) в scene_hdr ПОСЛЕ bloom, ДО present (не блумится).
    // Своя глубина (main_depth с CLEAR — z-пространство UI отдельное), __TextAtlas как глобалка.
    void SetUIPass(EngineContext* ctx);

    // Финальный проход: blit HDR-сцены (scene_hdr) в свопчейн с конвертацией формата.
    // Регистрируется последним (приоритет 30). Тонмаппинг появится на этапе bloom-composite.
    void SetPresentPass(EngineContext* ctx);

    // Ресайз экранных таргетов (scene_hdr/emission/bloom/depth) вынесен в ИНСТРУКЦИИ ресайза
    // TextureManager (зарегистрированы в _SetDefaultCommonResources), исполняемые render-потоком
    // через tm->ExecuteResizeInstructions — отдельной функции здесь больше нет.

    // Push-константы bloom-программ (один layout на все: down/up/composite). Раскладка совпадает с
    // cbuffer BloomParams в шейдерах comp/bloom_*.comp.hlsl.
    struct alignas(16) BloomParams {
        uint32_t useKaris  = 0;     // 1 — Karis-average вместо весов Jimenez (гасит fireflies)
        // Значит РАЗНОЕ у разных шейдеров: у prefilter — вклад сцены, у composite — силу свечения.
        // Поэтому в состоянии прохода (BloomState) это два отдельных поля, а не одно.
        float    intensity = 0.0f;
        float    threshold = 0.0f;  // prefilter: порог яркости, ниже которого сцена не блумит
        float    knee      = 0.0f;  // prefilter: ширина мягкого колена вокруг порога
        // prefilter: потолок вклада СЦЕНЫ в пирамиду (в единицах яркости сцены, как threshold).
        // Эмиссию не ограничивает. 0 = сценовый bloom выключен.
        float    clampMax  = 0.0f;
    };

    // СОСТОЯНИЕ прохода bloom (ComputePassStep::state). Не cbuffer: раскладку под шейдер каждая
    // программа собирает у себя (push-функции в наборе шейдеров игры), а здесь — то, чем проход
    // настраивают. Поэтому вклад сцены и сила свечения — РАЗНЫЕ поля: в BloomParams они делят
    // один intensity, но означают разное у prefilter и у composite.
    // Дефолты живут здесь, в member-инициализаторах: с них же MakeParamsSpec снимает defaults.
    // alignas(16) — дань общей фабрике схем: она требует кратности 16, потому что у второго
    // домена (params материала) блоб уезжает в cbuffer. Само состояние прохода в GPU не идёт.
    struct alignas(16) BloomState {
        float threshold = 1.2f;            // ПОЛ яркости сцены: ниже него не блумит ничего (диффуз ≤1)
        float knee = 0.9f;                 // ширина гладкого разгона ВВЕРХ от порога
        float scene_contribution = 0.135f;   // prefilter: доля вклада сцены; эмиссия идёт в полную силу
        // Потолок яркости, с которой сцена входит в пирамиду: сильнее этого гало не бывает.
        // Единственная нелинейность, выравнивающая режимы power 3 и power 50: threshold задаёт,
        // ГДЕ гало появляется, clamp — верх; scene_contribution после него — общая сила.
        float halo_clamp = 4.0f;
        float glow_intensity = 0.5f;       // composite: сила подмешивания bloom в scene_hdr
        // Karis-average: свёртка 13 тапов с весом 1/(1+L) вместо весов Jimenez. Гасит firefly'и
        // от одиночных сверхъярких пикселей ценой энергии ярких точек. Ниже — ДВА независимых
        // выключателя, по одному на этап пирамиды: на первом (prefilter) он и задуман, на
        // остальных уровнях обычно не нужен — но ветка в bloom_down есть, и теперь достижима.
        uint32_t karis_prefilter = 1;      // 1 — как было (в шейдере он был зашит безусловно)
        uint32_t karis_down = 0;           // 0 — как было (флаг всегда пушился нулём)
    };
    // Имя схемы BloomState в ParamsSpecRegistry::Passes() (регистрирует SetDefaultBloomPass).
    inline const std::string BLOOM_STATE = "BloomState";

    // Создаёт compute-проход bloom: пирамида downsample→upsample по эмиссии + composite/tonemap в
    // scene_hdr. Должна вызываться ПОСЛЕ main/transparent/debug (читает их результат) и до present.
    void SetDefaultBloomPass(EngineContext* ctx);

    // GPU-каллинг с компактацией (culling_pib.comp = scatter). Одна программа на группу камер:
    // раскладка = CullParams шейдера. Никакого is_shadow — группу задаёт push + камерный буфер.
    struct alignas(16) CullingPibUniform {
        uint32_t range_start;      // первая PIB-запись группы (её сегмент во входном PIB)
        uint32_t range_count;      // сколько записей (= размер диспатча)
        uint32_t num_blocks;       // число камер группы (Cameras[0..num_blocks))
        uint32_t cmd_base;         // база региона группы в индиректе, в командах
        uint32_t commands;         // команд на камеру = страйд блока внутри региона
    };
    // culling_clear.comp: обнуляет num_instances всех (камера,команда) перед scatter.
    struct alignas(16) CullingClearUniform {
        uint32_t total_slots;      // сумма по группам cams*commands (RenderSnap::Regions)
    };
    void SetDefaultCullingPass(EngineContext* ctx);

    inline const std::string SHADOW_MOMENTS_ARRAY = "shadow_moments_array";
    inline const std::string SHADOW_MOMENTS_BLUR_TEMP = "shadow_moments_single_temp";
}