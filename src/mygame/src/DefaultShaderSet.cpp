#include "PCH.h"
#include "DefaultShaderSet.h"
#include "LightDataModule.h"
#include "TransformDataModule.h"
#include "DefaultRenderPassSet.h"
#include "FractalUpdateSet.h"
#include "PositionStructure.h"

using namespace ShaderBase;   // POSITION/UV/... в раскладках вершин
#include "EngineContext.h"
#include "BufferManager.h"
#include "ShaderData.h"
#include "ShaderManager.h"
#include "ParamsSpec.h"
#include "RenderManager.h"
#include "BatchBuilder.h"
#include "RenderSnapshot.h"


namespace DefaultShaderProgramSet
{
    // compute (render-программы больше не создаются кодом — идут из shaders.json)
    bool culling_pib_inited = false;
    bool shadow_blur_inited = false;

}

// Единая точка регистрации СВОИХ push-констант — ОДИН РАЗ на инициализации (см. заголовок).
// Sp приезжают из shaders.json своей сцены; какая из них загружена сейчас — здесь не важно.
void DefaultShaderProgramSet::RegisterShaderFuncs(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();

    // Пуши движковых sp (ShadowCaster/Wireframe) регистрирует сам движок вместе с их
    // созданием — Engine::InitDefaultShaders. Игре остаются только её собственные.
    //
    // Фрактальные фоны: у каждой сцены свой sp, имена разные — регистрируем обе функции разом,
    // разбор имени сцены больше не нужен. Тело MAIN_PASS передаёт push_func nullptr вместо данных →
    // типизированная форма (разыменовывает raw) не годится; берём сырую, данные строим в лямбде
    // (fragment slot 0 → b0, space3).
    sm->CreatePushFunc("Fractal", [](const PushConstantBinder& b, const void*) {
        FractalUpdateSet::FractalPushData d{};   // max_steps по умолчанию — потолок рэймарча
        d.time = (float)SDL_GetTicks() / 1000.0f;
        b.PushFragment(d);
    });

    sm->CreatePushFunc("Mandelbrot", [](const PushConstantBinder& b, const void*) {
        FractalUpdateSet::FractalPushData d{};
        d.time = (float)SDL_GetTicks() / 1000.0f;
        // Потолок итераций пикселя = длина референс-орбиты: глубже неё дельты всё
        // равно не уходят (ре-базирование заворачивает m на начало орбиты).
        d.max_steps = FractalUpdateSet::MANDELBROT_ORBIT_MAX;
        b.PushFragment(d);
    });
}

namespace {
    // Регион прохода в штампе слота; прохода нет — пустой регион, диспатч выйдет нулевым.
    PassRegion RegionOfPass(PassManager* pm, uint32_t pass_ordinal, uint8_t slot)
    {
        const PassRegions& regions = pm->AskRegions(slot);
        if (pass_ordinal < regions.per_pass.size()) {
            return regions.per_pass[pass_ordinal];
        }
        return PassRegion{};
    }

    // Culling-программа ОДНОГО прохода. У всех проходов она одинакова кроме камерного буфера:
    // числа своего региона программа берёт из штампа PassManager по ординалу прохода, а сам
    // ординал стабилен (MainInit идёт после FillRenderPasses). Новый проход = ещё один вызов.
    void CreateCullingProgram(EngineContext* ctx, ShaderManager* sm, PassManager* pm,
                              const std::string& program_name, const RenderPassName& pass_name,
                              BufferDataName camera_buffer)
    {
        namespace RP = DefaultRenderPassNamespace;
        using namespace DefaultBuffersNames;

        uint32_t pass_ordinal = UINT32_MAX;
        RenderPassStep* pass = pm->GetRenderPassStep(pass_name);
        if (pass) {
            pass_ordinal = pass->ordinal;
        }

        ctx->CreateComputeShaderProgram(program_name, "culling_pib_cs",
            { DEFAULT_OUT_PIB_BUFFER, DEFAULT_INDIRECT_BUFFER },
            { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_CMD_BUFFER, DEFAULT_BOUND_SPHERE_BUFFER,
              camera_buffer, DEFAULT_TRANSFORM_BUFFER },   // ro t0..t4 (Cameras = t3)
            {}, {}, {},
            RP::CULLING_PASS, /*dont_save=*/true);

        sm->CreateComputePushFunc<RP::CullingPibUniform>(program_name,
            [pm, pass_ordinal](const PushConstantBinder& binder, RP::CullingPibUniform data) {
            const PassRegion region = RegionOfPass(pm, pass_ordinal, binder.slot);
            data.range_start = region.first_pib;
            data.range_count = region.pib;
            data.num_blocks  = region.command_blocks_count;
            data.cmd_base    = region.cmd_base;
            data.commands    = region.commands;
            binder.Push(0, data);
        });

        sm->CreateDispatchFunc<RP::DummyDispatchData>(program_name,
            [pm, pass_ordinal](DispatchSizeBinder& binder, RP::DummyDispatchData) {
            const PassRegion region = RegionOfPass(pm, pass_ordinal, binder.slot);
            uint32_t records = region.pib;
            if (region.command_blocks_count == 0) {
                records = 0;   // блоков у прохода нет (напр. ни одной теневой камеры) — работы нет
            }
            binder.element_count = { records, 1, 1 };
        });
    }
}

void DefaultShaderProgramSet::SetCullingPibPrograms(EngineContext* ctx)
{
    // push/dispatch регистрируем в РЕЕСТРЕ по имени программы (не полем csp):
    // загрузка сцены пересоздаёт csp, и реестр вешает функции на неё сам.
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultBuffersNames;
    if (culling_pib_inited) {
        SDL_Log("Culling PIB program already initialized.");
        return;
    }

    namespace RP = DefaultRenderPassNamespace;
    PassManager* pm = ctx->GetPassManager();

    // CSD (culling_clear_cs/culling_pib_cs) грузятся из сцены (shaders.json). Здесь — только
    // сами compute-программы (держат указатели на буферы, не сериализуются) + их push/dispatch.
    // csp хранит cs_name; резолв в CSD — на сборке compute-пайплайна (после LoadScene).

    // (1) CLEAR — обнуляет num_instances ВСЕХ (камера,команда) перед scatter. Создаётся ПЕРВОЙ →
    // в CULLING_PASS это shader_batch[0], SDL барьерит между compute-пассами → scatter видит нули.
    ComputeShaderProgram* csp_clear = ctx->CreateComputeShaderProgram("csp_culling_clear", "culling_clear_cs",
        { DEFAULT_INDIRECT_BUFFER },   // rw (u0)
        {}, {}, {}, {},
        RP::CULLING_PASS, /*dont_save=*/true);
    sm->CreateComputePushFunc<RP::CullingClearUniform>("csp_culling_clear",
        [pm](const PushConstantBinder& binder, RP::CullingClearUniform data) {
        data.total_slots = pm->AskRegions(binder.slot).total_commands;
        binder.Push(0, data);
    });
    sm->CreateDispatchFunc<RP::DummyDispatchData>("csp_culling_clear",
        [pm](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        binder.element_count = { pm->AskRegions(binder.slot).total_commands, 1, 1 };
    });

    // Скаттер: программа НА ПРОХОД с батчами, отличаются только имя и камерный буфер.
    CreateCullingProgram(ctx, sm, pm, "csp_cull_shadow",      RP::SHADOW_PASS,      DEFAULT_LIGHT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_main",        RP::MAIN_PASS,        DEFAULT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_transparent", RP::TRANSPARENT_PASS, DEFAULT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_debug",       RP::DEBUG_PASS,       DEFAULT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_ui",          RP::UI_PASS,          DEFAULT_CAMERA_BUFFER);

    culling_pib_inited = true;
}

void DefaultShaderProgramSet::SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm)
{
    // push/dispatch регистрируем в РЕЕСТРЕ по имени программы (не полем csp):
    // загрузка сцены пересоздаёт csp, и реестр вешает функции на неё сам.
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultRenderPassNamespace;
    if (shadow_blur_inited) {
        SDL_Log("Shadow blur programs already initialized.");
        return;
    }


    auto moments_atlas = ctx->GetTextureAtlas(SHADOW_MOMENTS_ARRAY);
    auto blur_temp_atlas = ctx->GetTextureAtlas(SHADOW_MOMENTS_BLUR_TEMP);
    uint32_t LAYER_COUNT = moments_atlas->layers;

    for (uint32_t L = 0; L < LAYER_COUNT; ++L) {
        std::string name_h = "csp_shadow_blur_h_" + std::to_string(L);
        ComputeShaderProgram* csp_h = ctx->CreateComputeShaderProgram(name_h, "shadow_blur_h_cs",
            {},                                       // rw buffers
            {},                                       // ro buffers
            { { SHADOW_MOMENTS_BLUR_TEMP, 0, 0 } },   // rw textures
            {},                                       // ro storage textures
            { SHADOW_MOMENTS_ARRAY },                 // samplers
            SHADOW_BLUR_PASS, /*dont_save=*/true);

        sm->CreateComputePushFunc<ShadowBlurUniform>(name_h,
            [L](const PushConstantBinder& binder, ShadowBlurUniform data) {
            data.layerIndex = L;
            binder.Push(0, data);
        });
        sm->CreateDispatchFunc<DummyDispatchData>(name_h,
            [L, blur_temp_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(binder.slot, L))
                binder.element_count = { blur_temp_atlas->width, blur_temp_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });

        std::string name_v = "csp_shadow_blur_v_" + std::to_string(L);
        ComputeShaderProgram* csp_v = ctx->CreateComputeShaderProgram(name_v, "shadow_blur_v_cs",
            {},
            {},
            { { SHADOW_MOMENTS_ARRAY, 0, L } },       // rw textures
            {},
            { SHADOW_MOMENTS_BLUR_TEMP },             // samplers
            SHADOW_BLUR_PASS, /*dont_save=*/true);

        sm->CreateDispatchFunc<DummyDispatchData>(name_v,
            [L, moments_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(binder.slot, L))
                binder.element_count = { moments_atlas->width, moments_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });
    }

    shadow_blur_inited = true;
}

void DefaultShaderProgramSet::SetBloomPrograms(EngineContext* ctx)
{
    // push/dispatch регистрируем в РЕЕСТРЕ по имени программы (не полем csp):
    // загрузка сцены пересоздаёт csp, и реестр вешает функции на неё сам.
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultRenderPassNamespace;
    static bool inited = false;
    if (inited) { SDL_Log("Bloom shader programs already initialized."); return; }

    // CSD (bloom_*_cs) грузятся из сцены (shaders.json) — здесь только сами compute-программы
    // (указатели на атласы, не сериализуются) + push/dispatch; csp резолвит cs_name на пайплайне.

    // Пирамида — BLOOM_LEVELS ОТДЕЛЬНЫХ текстур "bloom_L<i>" (см. _SetDefaultCommonResources):
    // dst-уровень биндится RW-storage, src-уровень — сэмплером. Отдельные текстуры исключают
    // одновременный RW+sampled бинд одной (layout-ошибки валидации на общей мип-цепочке).
    // Размер диспатча — по живому атласу уровня (ресайз меняет width/height внутри атласа).
    auto L = [](uint32_t i) { return "__bloom_L" + std::to_string(i); };

    ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
        "bloom_down_0", "bloom_prefilter_cs",
        {}, {},
        { { L(0), 0, 0 } },                                            // rw: bloom_L0
        {},
        { std::string("scene_hdr"), std::string("scene_emission") },   // sampler t0/s0, t1/s1
        BLOOM_PASS, /*dont_save=*/true);
    // Значения больше не литералы здесь: программа СОБИРАЕТ свой cbuffer из состояния прохода
    // (BloomState), которое лежит в шаге и правится редактором.
    sm->CreateComputePushFunc<BloomState>("bloom_down_0",[](const PushConstantBinder& b, BloomState st) {
        BloomParams d{};
        d.threshold = st.threshold;
        d.knee      = st.knee;
        d.intensity = st.scene_contribution;   // у prefilter intensity = вклад СЦЕНЫ
        d.clampMax  = st.halo_clamp;
        d.useKaris  = st.karis_prefilter;
        b.Push(0, d);
    });
    {
        TextureAtlas* dst = ctx->GetTextureAtlas(L(0));
        sm->CreateDispatchFunc<DummyDispatchData>("bloom_down_0",[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Downsample: уровень i-1 → уровень i (1..N-1). Источник — sampler соседнего уровня. ---
    for (uint32_t i = 1; i < BLOOM_LEVELS; ++i) {
        const std::string down_name = "bloom_down_" + std::to_string(i);
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            down_name, "bloom_down_cs",
            {}, {},
            { { L(i), 0, 0 } },   // rw: bloom_L<i>
            {},
            { L(i - 1) },         // combined sampler: предыдущий (вдвое крупнее) уровень
            BLOOM_PASS, /*dont_save=*/true);
        // Из состояния берёт только выключатель Karis: остальное bloom_down не читает.
        sm->CreateComputePushFunc<BloomState>(down_name,[](const PushConstantBinder& b, BloomState st) {
            BloomParams d{}; d.useKaris = st.karis_down; b.Push(0, d);
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L(i));
        sm->CreateDispatchFunc<DummyDispatchData>(down_name,[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Upsample (tent, аддитивно): уровень i+1 → += уровень i, от мелкого к крупному. ---
    for (int i = (int)BLOOM_LEVELS - 2; i >= 0; --i) {
        const std::string up_name = "bloom_up_" + std::to_string(i);
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            up_name, "bloom_up_cs",
            {}, {},
            // rw: bloom_L<i>. Tent-фильтр читает СОСЕДНИЕ тексели того же уровня, пока другие потоки
            // диспатча их пишут → нужен SIMULTANEOUS (не выводится из формы бинда — ручной тег).
            { { .texture_atlas = L((uint32_t)i), .need_simultaneous = true } },
            {},
            { L((uint32_t)i + 1) },         // combined sampler: следующий (вдвое мельче) уровень
            BLOOM_PASS, /*dont_save=*/true);
        // bloom_up не читает из cbuffer'а ничего, но слот пушить обязан.
        sm->CreateComputePushFunc<BloomState>(up_name,[](const PushConstantBinder& b, BloomState) {
            b.Push(0, BloomParams{});
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L((uint32_t)i));
        sm->CreateDispatchFunc<DummyDispatchData>(up_name,[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Composite: scene_hdr += bloom_L0 * intensity (hue-preserving clip) ---
    {
        auto dst = ctx->GetTextureAtlas(std::string("scene_hdr"));
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_composite", "bloom_composite_cs",
            {}, {},
            { { std::string("scene_hdr"), 0, 0 } },   // rw: scene_hdr (RMW)
            {},
            { L(0) },                                 // combined sampler: bloom_L0
            BLOOM_PASS, /*dont_save=*/true);
        sm->CreateComputePushFunc<BloomState>("bloom_composite",[](const PushConstantBinder& b, BloomState st) {
            BloomParams d{};
            d.intensity = st.glow_intensity;   // у composite intensity = сила свечения
            b.Push(0, d);
        });
        sm->CreateDispatchFunc<DummyDispatchData>("bloom_composite",[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    inited = true;
}
