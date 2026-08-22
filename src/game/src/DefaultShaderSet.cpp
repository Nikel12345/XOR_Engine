#include "PCH.h"
#include "DefaultShaderSet.h"
#include "LightDataModule.h"
#include "TransformDataModule.h"
#include "DefaultRenderPassSet.h"
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

namespace {
    // shadow_pib слота = сумма инстансов SHADOW_PASS в СЛЕПКЕ раскладки слота = граница PIB
    // между теневыми записями [0, sp) и остальными [sp, N). Она же first_instance первой
    // не-теневой команды (FinalizeOffsets нумерует по проходам, shadow первый по pass_index=10).
    // Так каждая scatter-программа берёт свой диапазон PIB.
    uint32_t ShadowPib(const RenderSnap::BatchLayout* layout, uint32_t shadow_ordinal)
    {
        if (!layout || shadow_ordinal >= layout->passes.size()) return 0u;
        return layout->passes[shadow_ordinal].num_instances;
    }
}

namespace DefaultShaderProgramSet
{
    // compute (render-программы больше не создаются кодом — идут из shaders.json)
    bool culling_pib_inited = false;
    bool shadow_blur_inited = false;

}

void DefaultShaderProgramSet::SetCullingPibPrograms(EngineContext* ctx, LightDataModule* ldm)
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
    PassManager*   pm = ctx->GetPassManager();
    BatchBuilder*  bb = ctx->GetBatchBuilder();

    // CSD (culling_clear_cs/culling_pib_cs) грузятся из сцены (shaders.json). Здесь — только
    // сами compute-программы (держат указатели на буферы, не сериализуются) + их push/dispatch.
    // csp хранит cs_name; резолв в CSD — на сборке compute-пайплайна (после LoadScene).

    // Ordinal теневого прохода в слепке раскладки (стабилен после FillRenderPasses) —
    // ключ для ShadowPib. Все лямбды ниже читают ТОЛЬКО слепки слота binder.slot.
    RenderPassStep* shadow_rp = pm->GetRenderPassStep(RP::SHADOW_PASS);
    const uint32_t shadow_ord = shadow_rp ? shadow_rp->ordinal : 0xFFFFFFFFu;

    // (1) CLEAR — обнуляет num_instances ВСЕХ (камера,команда) перед scatter. Создаётся ПЕРВОЙ →
    // в CULLING_PASS это shader_batch[0], SDL барьерит между compute-пассами → scatter видит нули.
    ComputeShaderProgram* csp_clear = ctx->CreateComputeShaderProgram("csp_culling_clear", "culling_clear_cs",
        { DEFAULT_INDIRECT_BUFFER },   // rw (u0)
        {}, {}, {}, {},
        RP::CULLING_PASS, /*dont_save=*/true);
    sm->CreateComputePushFunc<RP::CullingClearUniform>("csp_culling_clear",
        [ldm, bb](const PushConstantBinder& binder, RP::CullingClearUniform data) {
        data.total_slots = (1 + ldm->AskNumLightCameras(binder.slot)) * bb->AskNumCommands(binder.slot);
        binder.Push(0, data);
    });
    sm->CreateDispatchFunc<RP::DummyDispatchData>("csp_culling_clear",
        [ldm, bb](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        binder.element_count = { (1 + ldm->AskNumLightCameras(binder.slot)) * bb->AskNumCommands(binder.slot), 1, 1 };
    });

    // Общий шейдер scatter'а для всех групп камер (разные программы = разные камерные буферы).

    // (2) ИГРОК — камера игрока (блок 0, num_blocks=1), main-записи [shadow_pib, N). Cameras=CAMERA_BUFFER (t3).
    ComputeShaderProgram* csp_player = ctx->CreateComputeShaderProgram("csp_cull_player", "culling_pib_cs",
        { DEFAULT_OUT_PIB_BUFFER, DEFAULT_INDIRECT_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_CMD_BUFFER, DEFAULT_BOUND_SPHERE_BUFFER,
          DEFAULT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },   // ro t0..t4 (Cameras = игрок)
        {}, {}, {},
        RP::CULLING_PASS, /*dont_save=*/true);
    sm->CreateComputePushFunc<RP::CullingPibUniform>("csp_cull_player",
        [bb, shadow_ord](const PushConstantBinder& binder, RP::CullingPibUniform data) {
        const RenderSnap::BatchLayout* lo = bb->AskLayout(binder.slot);
        uint32_t sp = ShadowPib(lo, shadow_ord);
        uint32_t N  = lo ? lo->num_instances : 0u;
        data.range_start = sp;
        data.range_count = (N > sp) ? (N - sp) : 0u;
        data.block_base = 0u;
        data.num_blocks = 1u;
        data.block_stride = N;
        data.total_commands = lo ? lo->num_commands : 0u;
        binder.Push(0, data);
    });
    sm->CreateDispatchFunc<RP::DummyDispatchData>("csp_cull_player",
        [bb, shadow_ord](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        const RenderSnap::BatchLayout* lo = bb->AskLayout(binder.slot);
        uint32_t sp = ShadowPib(lo, shadow_ord);
        uint32_t N  = lo ? lo->num_instances : 0u;
        binder.element_count = { (N > sp) ? (N - sp) : 0u, 1, 1 };
    });

    // (3) СВЕТ — световые камеры (блоки 1..L, num_blocks=L), теневые записи [0, shadow_pib).
    // Cameras=LIGHT_CAMERA_BUFFER (t3). При L=0 диспатч 0 → пропуск.
    ComputeShaderProgram* csp_light = ctx->CreateComputeShaderProgram("csp_cull_light", "culling_pib_cs",
        { DEFAULT_OUT_PIB_BUFFER, DEFAULT_INDIRECT_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_CMD_BUFFER, DEFAULT_BOUND_SPHERE_BUFFER,
          DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },   // ro t0..t4 (Cameras = свет)
        {}, {}, {},
        RP::CULLING_PASS, /*dont_save=*/true);
    sm->CreateComputePushFunc<RP::CullingPibUniform>("csp_cull_light",
        [ldm, bb, shadow_ord](const PushConstantBinder& binder, RP::CullingPibUniform data) {
        const RenderSnap::BatchLayout* lo = bb->AskLayout(binder.slot);
        uint32_t L  = ldm->AskNumLightCameras(binder.slot);
        uint32_t sp = ShadowPib(lo, shadow_ord);
        data.range_start = 0u;
        data.range_count = (L > 0) ? sp : 0u;
        data.block_base = 1u;
        data.num_blocks = L;
        data.block_stride = lo ? lo->num_instances : 0u;
        data.total_commands = lo ? lo->num_commands : 0u;
        binder.Push(0, data);
    });
    sm->CreateDispatchFunc<RP::DummyDispatchData>("csp_cull_light",
        [ldm, bb, shadow_ord](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        uint32_t L = ldm->AskNumLightCameras(binder.slot);
        binder.element_count = { (L > 0) ? ShadowPib(bb->AskLayout(binder.slot), shadow_ord) : 0u, 1, 1 };
    });

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
