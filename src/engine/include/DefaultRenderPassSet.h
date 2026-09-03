#pragma once
#include "TextureData.h"
class EngineContext;
class LightDataModule;
class BatchBuilder;
class PassManager;
namespace DefaultRenderPassNamespace
{
    inline constexpr const char* DEPTH_PASS = "_DefaultDepthRenderPass";
    inline constexpr const char* MAIN_PASS = "_DefaultMainRenderPass";
    inline constexpr const char* TRANSPARENT_PASS = "_DefaultTransparentRenderPass";
    inline constexpr const char* DEBUG_PASS = "_DefaultDebugRenderPass";
    inline constexpr const char* PRESENT_PASS = "_DefaultPresentPass";
    inline constexpr const char* BLOOM_PASS = "_DefaultBloomPass";
    inline constexpr const char* AO_PASS = "_DefaultAOPass";
    inline constexpr const char* FOG_PASS = "_DefaultFogPass";
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

    // Число источников света в LIGHT_BUFFER слота — состояние MAIN/TRANSPARENT прохода, едет
    // push-константой в его лайтящие программы (fragment slot 0 → b0, space3). Счётчик обязан
    // приходить ЯВНО: размер буфера для этого не годится (он только растёт, см.
    // LightDataModule.h), а рендер-поток берёт значение из слепка слота, не из ECS.
    // КОНТРАКТ: программа, чей fs включает лайтинг-базу (main_pass.frag.hlsl / transparent.
    // frag.hlsl), ОБЯЗАНА зарегистрировать пуш этой структуры (CreatePushFunc по имени sp) —
    // он занимает b0, а UVL/params/раскладка съезжают следом (uvl_slot = binder.frag_count).
    // Забыли — материальные униформы уедут на регистр ниже, это видно сразу.
    struct alignas(16) LightCountPushData
    {
        Uint32 light_count;
    };

    struct ShadowBlurUniform { uint32_t layerIndex; };
    struct DummyDispatchData {};
    void SetDefaultShadowBlurPass(EngineContext* ctx);

    // PassSystem: общие ресурсы дефолтного набора проходов (разделяемый depth-таргет, его формат).
    // Должна вызываться ПЕРЕД Set*Pass, которые их потребляют (main/transparent/debug).
    void _SetDefaultCommonResources(EngineContext* ctx, uint32_t width, uint32_t height);

    // ldm — источник счётчика источников света слота (AskNumLights): проход кладёт его в своё
    // состояние, откуда push-функции программ забирают его в b0 (см. LightCountPushData).
    void SetDefaultMainRenderPass(EngineContext* ctx, LightDataModule* ldm);
    void SetDefaultMainRenderPass(EngineContext* ctx, SDL_GPUDevice* dev, SDL_Window* win);

    // Состояние DEBUG_PASS: цвет рамок коллайдеров. Тело прохода его не трогает — это чистая
    // настройка, поэтому у прохода есть схема (имя ниже) и он редактируется.
    struct alignas(16) DebugColliderPushData { float color[4] = { 0.0f, 1.0f, 0.2f, 1.0f }; };
    inline const std::string DEBUG_COLLIDER_STATE = "DebugCollider";
    void SetDebugColliderPass(EngineContext* ctx);

    void SetTransparentPass(EngineContext* ctx, LightDataModule* ldm);   // ldm — см. SetDefaultMainRenderPass

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

    // Экранное затенение непрямого света (SSAO). Считается по ГЛУБИНЕ main-прохода (нормаль
    // восстанавливается из неё же), применяется вычитанием из scene_hdr доли, которую main-проход
    // отложил в scene_ambient — третий MRT-таргет. Прямой свет и эмиссия не затрагиваются.
    //
    // Состояние прохода = его настройки (радиус/сила/контраст/bias), одно на все четыре программы:
    // они делят один cbuffer AOParams. intensity = 0 выключает эффект целиком, не убирая проход.
    struct alignas(16) AOState {
        float radius    = 0.5f;    // радиус полусферы в МИРОВЫХ единицах
        float intensity = 1.0f;    // сила затенения; 0 — выключено
        float power     = 1.5f;    // контраст кривой: >1 темнит углы и осветляет открытое
        // Порог самозатенения как ДОЛЯ view-глубины точки (не абсолют): шум восстановленной
        // нормали и квантование буфера глубины растут с дистанцией так же, поэтому абсолютный
        // порог пришлось бы перекручивать под каждый ракурс. Без него плоскость затеняет себя.
        float bias      = 0.01f;
    };
    inline const std::string AO_STATE = "AOState";

    inline const std::string SCENE_AMBIENT = "scene_ambient";
    inline const std::string SSAO_TEXTURE  = "__ssao";
    inline const std::string SSAO_TEMP     = "__ssao_temp";

    // Проход-compute между MAIN (20) и TRANSPARENT (22): к моменту его исполнения глубина и
    // ambient непрозрачной геометрии готовы, а bloom (26) увидит уже затенённую сцену.
    void SetDefaultAOPass(EngineContext* ctx);

    // Атмосфера кадра: линейный туман по ДИСТАНЦИИ от камеры. Ничего ближе start_distance, полный
    // цвет тумана дальше full_distance, равномерный подъём между ними. Высоты в модели нет — стена
    // встаёт одинаково во все стороны, а не стелется по низинам.
    //
    // Небо туманится наравне с геометрией (его глубина = дальняя плоскость), поэтому при включённом
    // тумане скайбокса не видно — это принятая цена за отсутствие разрыва по горизонту.
    //
    // max_opacity = 0 = эффект выключен, проход становится тождеством (шаг при этом остаётся).
    struct alignas(16) FogState {
        float color[3]        = { 0.05f, 0.05f, 0.05f };   // цвет тумана, как есть
        float start_distance  = 550.0f;    // ближе — тумана нет вовсе, юниты
        float full_distance   = 1450.0f;   // дальше — только цвет тумана
        float max_opacity     = 0.9f;    // потолок: 1 — даль исчезает полностью
    };
    inline const std::string FOG_STATE = "FogState";

    // Проход-compute ПОСЛЕ bloom (26), до UI (28) — последний, кто трогает кадр сцены.
    //
    // Именно после bloom, и это не вкусовщина: bloom строит пирамику из scene_emission, который
    // экранный проход не гасит. Стоя перед ним, туман честно доводил пиксель до цвета стены, а
    // composite (26) подмешивал поверх НЕзатуманенное свечение — далёкая геометрия переставала
    // исчезать и оставалась размытым свечением поверх стены. Здесь гасится уже собранный кадр
    // вместе со свечением, и отдельная запись в эмиссию не нужна.
    //
    // Побочно туман достаётся и прозрачным (22) — но по глубине того, что ЗА ними: глубину они не
    // пишут (см. "LitTransparent" в Engine::InitDefaultShaders). Для дали это ровно то, что надо;
    // близкое стекло на фоне неба уйдёт в туман сильнее положенного. Честное лечение — считать
    // туман в их фрагментнике, отдельной задачей.
    //
    // Ещё следствие: туман ложится ПОСЛЕ тонмаппинга bloom-композита, то есть fog_color попадает
    // в кадр ровно тем RGB, что задан в состоянии, без сжатия HDR.
    void SetDefaultFogPass(EngineContext* ctx);

    // GPU-каллинг с компактацией (culling_pib.comp = scatter). Одна программа НА ПРОХОД с
    // батчами: раскладка = CullParams шейдера, регион и камерный буфер задаёт программа.
    struct alignas(16) CullingPibUniform {
        uint32_t range_start;      // первая PIB-запись группы (её сегмент во входном PIB)
        uint32_t range_count;      // сколько записей (= размер диспатча)
        uint32_t num_blocks;       // блоков региона; для блока b тестируется Cameras[b]
        uint32_t cmd_base;         // база региона группы в индиректе, в командах
        uint32_t commands;         // команд на камеру = страйд блока внутри региона
    };
    // culling_clear.comp: обнуляет num_instances всех (камера,команда) перед scatter.
    struct alignas(16) CullingClearUniform {
        uint32_t total_slots;      // PassRegions::total_commands слота — все блоки всех проходов
    };
    void SetDefaultCullingPass(EngineContext* ctx);

    inline const std::string SHADOW_MOMENTS_ARRAY = "shadow_moments_array";
    inline const std::string SHADOW_MOMENTS_BLUR_TEMP = "shadow_moments_single_temp";
}