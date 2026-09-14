#include "PCH.h"
#include "DefaultShaderSet.h"
#include "LightDataModule.h"
#include "TransformDataModule.h"
#include "DefaultRenderPassSet.h"
#include "PositionStructure.h"

using namespace ShaderBase;
#include "EngineContext.h"
#include "BufferManager.h"
#include "ShaderData.h"
#include "ShaderManager.h"
#include "ParamsSpec.h"
#include "PassManager.h"
#include "BatchBuilder.h"
#include "RenderSnapshot.h"


namespace DefaultShaderProgramSet
{
    bool culling_pib_inited = false;
    bool shadow_blur_inited = false;

}

namespace {
    // Прохода нет — пустой регион, и диспатч у программы выйдет нулевым.
    PassRegion RegionOfPass(PassManager* pm, uint32_t pass_ordinal, uint8_t slot)
    {
        const PassRegions& regions = pm->AskRegions(slot);
        if (pass_ordinal < regions.per_pass.size()) {
            return regions.per_pass[pass_ordinal];
        }
        return PassRegion{};
    }

    // Culling-программа ОДНОГО прохода: у всех проходов она одинакова кроме камерного буфера, а
    // числа своего региона берёт из штампа PassManager по ординалу прохода. Ординал снимается здесь
    // и стабилен — MainInit идёт после FillRenderPasses.
    void CreateCullingProgram(EngineContext* ctx, ShaderManager* sm, PassManager* pm,
                              const std::string& program_name, const RenderPassName& pass_name,
                              BufferDataName camera_buffer, TextureAtlas* screen_target = nullptr,
                              bool invert_span = false)
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

        sm->CreateComputePushInstruction<RP::CullingPibUniform>(program_name,
            [pm, pass_ordinal, screen_target, invert_span](const PushConstantBinder& binder, RP::CullingPibUniform data) {
            const PassRegion region = RegionOfPass(pm, pass_ordinal, binder.frame);
            data.range_start = region.first_pib;
            data.range_count = region.pib;
            data.num_blocks  = region.command_blocks_count;
            data.cmd_base    = region.cmd_base;
            data.commands    = region.commands;
            // Состояние прохода одно на все программы каллинга, а порог в пикселях применим не
            // всем: у теней и UI своё разрешение. Кому не дали таргет — тому отсев выключен.
            if (screen_target) data.target_height = screen_target->height;
            else               data.min_screen_radius_px = 0.0f;
            data.invert_span = invert_span ? 1u : 0u;
            binder.Push(data);
        });

        sm->CreateDispatchInstruction<RP::DummyDispatchData>(program_name,
            [pm, pass_ordinal](DispatchSizeBinder& binder, RP::DummyDispatchData) {
            const PassRegion region = RegionOfPass(pm, pass_ordinal, binder.frame);
            uint32_t records = region.pib;
            if (region.command_blocks_count == 0) {
                records = 0;   // блоков у прохода нет (напр. ни одной теневой камеры) — работы нет
            }
            binder.element_count = { records, 1, 1 };
        });
    }
}

void DefaultShaderProgramSet::SetDefaultPushes(EngineContext* ctx)
{
    namespace RP = DefaultRenderPassNamespace;
    ShaderManager* sm = ctx->GetShaderManager();

    sm->RegisterPushKind<RP::LightCountPushData>("light_count", PushStage::Fragment,
        [](const PushConstantBinder& b, RP::LightCountPushData data) { b.Push(data); });

    sm->RegisterPushKind("uvl", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput& in) {
        if (!in.draw) return;
        if (!in.draw->texture_uvl.empty()) { b.Push(in.draw->texture_uvl); return; }
        // Пустую таблицу всё равно пушим: блок ОБЪЯВЛЕН, а пропуск оставил бы в слоте таблицу
        // предыдущего draw'а.
        const UVL_Block empty{};
        b.Push(empty);
    });
    sm->RegisterPushKind("material_params", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput& in) {
        // Пусто = материал не дал параметров шейдеру, который их объявил: ошибка материала,
        // пушить нечего.
        if (!in.draw || !in.draw->params || in.draw->params->empty()) return;
        b.Push(*in.draw->params);
    });
    sm->RegisterPushKind("variant_layout", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput& in) {
        if (!in.draw) return;
        b.Push(in.draw->variant_layout);
    });

    // Тот же ShadowPushData тело теневого прохода пушит ещё и в вершинник, прямым вызовом.
    sm->CreatePushInstruction<RP::ShadowPushData>("ShadowCaster", PushStage::Fragment,
        [](const PushConstantBinder& b, RP::ShadowPushData data) { b.Push(data); });
    sm->CreatePushInstruction<RP::DebugColliderPushData>("Wireframe", PushStage::Fragment,
        [](const PushConstantBinder& b, RP::DebugColliderPushData data) { b.Push(data); });
}

void DefaultShaderProgramSet::SetCullingPibPrograms(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultBuffersNames;
    if (culling_pib_inited) {
        SDL_Log("Culling PIB program already initialized.");
        return;
    }

    namespace RP = DefaultRenderPassNamespace;
    PassManager* pm = ctx->GetPassManager();

    // CLEAR обнуляет num_instances всех пар (камера, команда) и создаётся ПЕРВОЙ: в CULLING_PASS
    // это shader_batch[0], а между compute-пассами SDL ставит барьер, поэтому scatter видит нули.
    ComputeShaderProgram* csp_clear = ctx->CreateComputeShaderProgram("csp_culling_clear", "culling_clear_cs",
        { DEFAULT_INDIRECT_BUFFER },   // rw (u0)
        {}, {}, {}, {},
        RP::CULLING_PASS, /*dont_save=*/true);
    sm->CreateComputePushInstruction<RP::CullingClearUniform>("csp_culling_clear",
        [pm](const PushConstantBinder& binder, RP::CullingClearUniform data) {
        data.total_slots = pm->AskRegions(binder.frame).total_commands;
        binder.Push(data);
    });
    sm->CreateDispatchInstruction<RP::DummyDispatchData>("csp_culling_clear",
        [pm](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        binder.element_count = { pm->AskRegions(binder.frame).total_commands, 1, 1 };
    });

    // Скаттер: по программе НА ПРОХОД с батчами. Отсев по экранному размеру включён только у
    // MAIN — он один рисует непрозрачную массовку, ради которой режим и заведён.
    TextureAtlas* scene_hdr = ctx->GetTextureAtlas(std::string("scene_hdr"));
    CreateCullingProgram(ctx, sm, pm, "csp_cull_shadow",      RP::SHADOW_PASS,      DEFAULT_LIGHT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_main",        RP::MAIN_PASS,        DEFAULT_CAMERA_BUFFER, scene_hdr);
    // Сплат выключен вместе со своим проходом (Engine::Init); механизм invert_span остаётся в
    // шейдере и включается этой строкой.
    // CreateCullingProgram(ctx, sm, pm, "csp_cull_splat", RP::SPLAT_PASS, DEFAULT_CAMERA_BUFFER, scene_hdr, /*invert_span=*/true);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_transparent", RP::TRANSPARENT_PASS, DEFAULT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_debug",       RP::DEBUG_PASS,       DEFAULT_CAMERA_BUFFER);
    CreateCullingProgram(ctx, sm, pm, "csp_cull_ui",          RP::UI_PASS,          DEFAULT_CAMERA_BUFFER);

    culling_pib_inited = true;
}

void DefaultShaderProgramSet::SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm)
{
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

        sm->CreateComputePushInstruction<ShadowBlurUniform>(name_h,
            [L](const PushConstantBinder& binder, ShadowBlurUniform data) {
            data.layerIndex = L;
            binder.Push(data);
        });
        sm->CreateDispatchInstruction<DummyDispatchData>(name_h,
            [L, blur_temp_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(binder.frame, L))
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

        sm->CreateDispatchInstruction<DummyDispatchData>(name_v,
            [L, moments_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(binder.frame, L))
                binder.element_count = { moments_atlas->width, moments_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });
    }

    shadow_blur_inited = true;
}

void DefaultShaderProgramSet::SetAOPrograms(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultRenderPassNamespace;
    using namespace DefaultBuffersNames;
    static bool inited = false;
    if (inited) { SDL_Log("AO shader programs already initialized."); return; }

    // ПОРЯДОК СОЗДАНИЯ = порядок исполнения, и он здесь единственная связь между шагами: ssao
    // пишет __ssao, blur_h переносит его в __ssao_temp, blur_v — обратно, композит читает __ssao.
    // Переставить местами значит читать прошлый кадр.
    //
    // Камерный буфер нужен и блюру: билатеральный вес считается по ЛИНЕЙНОЙ глубине, а её достают
    // из буфера глубины членами proj.
    ctx->CreateComputeShaderProgram("ssao", "ssao_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEXTURE, 0, 0 } },   // rw
        {},
        { std::string("__main_depth") },
        AO_PASS, /*dont_save=*/true);

    // Разделимый блюр ping-pong'ом между двумя картами. SIMULTANEOUS не нужен: каждый шаг читает
    // ЧУЖУЮ текстуру, а пишет только свой тексель.
    ctx->CreateComputeShaderProgram("ssao_blur_h", "ssao_blur_h_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEMP, 0, 0 } },
        {},
        { SSAO_TEXTURE, std::string("__main_depth") },
        AO_PASS, /*dont_save=*/true);

    ctx->CreateComputeShaderProgram("ssao_blur_v", "ssao_blur_v_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEXTURE, 0, 0 } },
        {},
        { SSAO_TEMP, std::string("__main_depth") },
        AO_PASS, /*dont_save=*/true);

    // scene_hdr здесь только storage, ambient и AO только сэмплеры — одновременного
    // sampler+storage на одной текстуре нет.
    ctx->CreateComputeShaderProgram("ao_composite", "ao_composite_cs",
        {}, {},
        { { std::string("scene_hdr"), 0, 0 } },
        {},
        { SCENE_AMBIENT, SSAO_TEXTURE },
        AO_PASS, /*dont_save=*/true);

    const char* programs[] = { "ssao", "ssao_blur_h", "ssao_blur_v", "ao_composite" };
    for (const char* name : programs) {
        sm->CreateComputePushInstruction<AOState>(name, [](const PushConstantBinder& b, AOState st) {
            b.Push(st);
        });
    }

    {
        TextureAtlas* ao_tex = ctx->GetTextureAtlas(SSAO_TEXTURE);
        TextureAtlas* ao_tmp = ctx->GetTextureAtlas(SSAO_TEMP);
        TextureAtlas* hdr    = ctx->GetTextureAtlas(std::string("scene_hdr"));

        sm->CreateDispatchInstruction<DummyDispatchData>("ssao", [ao_tex](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { ao_tex->width, ao_tex->height, 1 };
        });
        sm->CreateDispatchInstruction<DummyDispatchData>("ssao_blur_h", [ao_tmp](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { ao_tmp->width, ao_tmp->height, 1 };
        });
        sm->CreateDispatchInstruction<DummyDispatchData>("ssao_blur_v", [ao_tex](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { ao_tex->width, ao_tex->height, 1 };
        });
        sm->CreateDispatchInstruction<DummyDispatchData>("ao_composite", [hdr](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { hdr->width, hdr->height, 1 };
        });
    }

    inited = true;
}

void DefaultShaderProgramSet::SetFogProgram(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultRenderPassNamespace;
    using namespace DefaultBuffersNames;
    static bool inited = false;
    if (inited) { SDL_Log("Fog shader program already initialized."); return; }

    // scene_hdr здесь только storage, глубина — только сэмплер: одновременного sampler+storage на
    // одной текстуре нет.
    ctx->CreateComputeShaderProgram("fog", "fog_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { std::string("scene_hdr"), 0, 0 } },   // rw
        {},
        { std::string("__main_depth") },
        FOG_PASS, /*dont_save=*/true);

    sm->CreateComputePushInstruction<FogState>("fog", [](const PushConstantBinder& b, FogState st) {
        b.Push(st);
    });

    {
        TextureAtlas* hdr = ctx->GetTextureAtlas(std::string("scene_hdr"));
        sm->CreateDispatchInstruction<DummyDispatchData>("fog", [hdr](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { hdr->width, hdr->height, 1 };
        });
    }

    inited = true;
}

void DefaultShaderProgramSet::SetBloomPrograms(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();
    using namespace DefaultRenderPassNamespace;
    static bool inited = false;
    if (inited) { SDL_Log("Bloom shader programs already initialized."); return; }

    // ПОРЯДОК СОЗДАНИЯ = порядок исполнения: prefilter, затем down по уровням вниз, затем up
    // снизу вверх, затем композит — каждый шаг читает то, что записал предыдущий.
    //
    // Пирамида — BLOOM_LEVELS ОТДЕЛЬНЫХ текстур "bloom_L<i>", а не мипы одной: dst-уровень
    // биндится RW-storage, src — сэмплером, и на общей мип-цепочке это ошибка layout-валидации.
    auto L = [](uint32_t i) { return "__bloom_L" + std::to_string(i); };

    ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
        "bloom_down_0", "bloom_prefilter_cs",
        {}, {},
        { { L(0), 0, 0 } },                                            // rw: bloom_L0
        {},
        { std::string("scene_hdr"), std::string("scene_emission") },   // sampler t0/s0, t1/s1
        BLOOM_PASS, /*dont_save=*/true);
    sm->CreateComputePushInstruction<BloomState>("bloom_down_0",[](const PushConstantBinder& b, BloomState st) {
        BloomParams d{};
        d.threshold = st.threshold;
        d.knee      = st.knee;
        d.intensity = st.scene_contribution;   // у prefilter intensity = вклад СЦЕНЫ
        d.clampMax  = st.halo_clamp;
        d.useKaris  = st.karis_prefilter;
        b.Push(d);
    });
    {
        TextureAtlas* dst = ctx->GetTextureAtlas(L(0));
        sm->CreateDispatchInstruction<DummyDispatchData>("bloom_down_0",[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    for (uint32_t i = 1; i < BLOOM_LEVELS; ++i) {
        const std::string down_name = "bloom_down_" + std::to_string(i);
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            down_name, "bloom_down_cs",
            {}, {},
            { { L(i), 0, 0 } },   // rw: bloom_L<i>
            {},
            { L(i - 1) },         // combined sampler: предыдущий (вдвое крупнее) уровень
            BLOOM_PASS, /*dont_save=*/true);
        sm->CreateComputePushInstruction<BloomState>(down_name,[](const PushConstantBinder& b, BloomState st) {
            BloomParams d{}; d.useKaris = st.karis_down; b.Push(d);
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L(i));
        sm->CreateDispatchInstruction<DummyDispatchData>(down_name,[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    for (int i = (int)BLOOM_LEVELS - 2; i >= 0; --i) {
        const std::string up_name = "bloom_up_" + std::to_string(i);
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            up_name, "bloom_up_cs",
            {}, {},
            // Tent-фильтр читает СОСЕДНИЕ тексели того же уровня, пока другие потоки диспатча их
            // пишут: отсюда SIMULTANEOUS, из формы бинда он не выводится.
            { { .texture_atlas = L((uint32_t)i), .need_simultaneous = true } },
            {},
            { L((uint32_t)i + 1) },         // combined sampler: следующий (вдвое мельче) уровень
            BLOOM_PASS, /*dont_save=*/true);
        // bloom_up не читает из cbuffer'а ничего, но слот пушить обязан.
        sm->CreateComputePushInstruction<BloomState>(up_name,[](const PushConstantBinder& b, BloomState) {
            b.Push(BloomParams{});
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L((uint32_t)i));
        sm->CreateDispatchInstruction<DummyDispatchData>(up_name,[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    {
        auto dst = ctx->GetTextureAtlas(std::string("scene_hdr"));
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_composite", "bloom_composite_cs",
            {}, {},
            { { std::string("scene_hdr"), 0, 0 } },   // rw: scene_hdr (RMW)
            {},
            { L(0) },                                 // combined sampler: bloom_L0
            BLOOM_PASS, /*dont_save=*/true);
        sm->CreateComputePushInstruction<BloomState>("bloom_composite",[](const PushConstantBinder& b, BloomState st) {
            BloomParams d{};
            d.intensity = st.glow_intensity;   // у composite intensity = сила свечения
            b.Push(d);
        });
        sm->CreateDispatchInstruction<DummyDispatchData>("bloom_composite",[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    inited = true;
}
