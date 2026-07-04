#include "PCH.h"
#include "DefaultShaderSet.h"
#include "LightDataModule.h"
#include "TransformDataModule.h"
#include "DefaultRenderPassSet.h"
#include "PositionStructure.h"
#include "EngineContext.h"

namespace DefaultShaderProgramSet
{
    // render
    bool render_main_inited = false;
    bool render_shadow_inited = false;
    bool render_transparent_inited = false;
    bool debug_collider_inited = false;

    // compute
    bool culling_zeros_inited = false;
    bool culling_count_inited = false;
    bool culling_offset_inited = false;
    bool culling_out_indirect_inited = false;
    bool culling_write_inited = false;

    bool shadow_blur_inited = false;

    // Общий VS для main и transparent пассов: один GPU-шейдер на оба (не плодим дубль).
    // Создаётся лениво при первом запросе; копия VertexShaderData переиспользует тот же
    // SDL_GPUShader*, поэтому передача по значению в CreateShaderProgram безопасна.
    static VertexShaderData main_pass_vs;
    static bool main_pass_vs_inited = false;

    static VertexShaderData GetMainPassVertexShader(EngineContext* ctx)
    {
        using namespace DefaultBuffersNames;
        if (!main_pass_vs_inited) {
            main_pass_vs = ctx->CreateVertexShader(
                "../engine/shaders_code/main_pass/main_pass.vert.hlsl",
                { { DEFAULT_VERTEX_BUFFER, &FMT_PosUVNormal, {POSITION, UV, NORMAL, TANGENT} } });
            main_pass_vs_inited = true;
        }
        return main_pass_vs;
    }
}

void DefaultShaderProgramSet::SetMainShaderProgram(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (render_main_inited) {
        SDL_Log("Main render shader programs already initialized.");
        return;
    }
    VertexShaderData vs = GetMainPassVertexShader(ctx);
    FragmentShaderData fs = ctx->CreateFragmentShader("../engine/shaders_code/main_pass/surface.hlsl");
	FragmentShaderData fs_debug = ctx->CreateFragmentShader("../engine/shaders_code/main_pass/debug_pass.frag.hlsl");
    ShaderProgramDescription* spd_main =
        ctx->CreateShaderProgramDescription("spd")
        ->BehavesAsOpaqueGeometry()->DoesNotCull()
        ;

    ctx->CreateShaderProgram("sp", spd_main, DefaultRenderPassNamespace::MAIN_PASS,
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
        fs, { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
        // Порядок ДОЛЖЕН совпадать с textures[] в прологе: albedo, normal, orm, emissive.
        { TextureSlotRole::Albedo, TextureSlotRole::Normal, TextureSlotRole::ORM, TextureSlotRole::Emissive }
    );

    render_main_inited = true;
 //   ShaderProgramDescription* spd_debug =
 //       sm->CreateShaderProgramDescription("spd_debug")
 //       ->UsedInRenderPass(pm->GetRenderPassStep("DEBUG_PASS"))
 //       ->BehavesAsOpaqueGeometry()->DoesNotCull()
 //       ;
 //
	//VertexShaderData vs_old = sm->CreateVertexShaderFromSPV("../engine/shaders_spv/main_pass.vert.spv");
	//FragmentShaderData fs_old = sm->CreateFragmentShaderFromSPV("../engine/shaders_spv/main_pass.frag.spv");
 //   sm->CreateShaderProgram("sp_debug", spd_debug, bm,
 //       vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
 //       fs_debug, { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
 //       { TextureSlotRole::Albedo, TextureSlotRole::Normal }
 //   );

}

void DefaultShaderProgramSet::SetDefaultShadowShaderProgram(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (render_shadow_inited) {
        SDL_Log("Shadow render shader programs already initialized.");
        return;
    }
	VertexShaderData vs_2 = ctx->CreateVertexShader("../engine/shaders_code/shadow_pass/shadow_pass.vert.hlsl", { { DEFAULT_VERTEX_BUFFER, &FMT_PosUVNormal, {POSITION} } });
	FragmentShaderData fs_2 = ctx->CreateFragmentShader("../engine/shaders_code/shadow_pass/shadow_pass.frag.hlsl");

    RasterizerStateBiasParams shadow_rsbp = {};
    shadow_rsbp.enable_depth_bias = true;
    shadow_rsbp.depth_bias_constant_factor = 1.0f;   // подбираешь
    shadow_rsbp.depth_bias_slope_factor = 2.0f;   // подбираешь
    shadow_rsbp.depth_bias_clamp = 0.0f;
    ShaderProgramDescription* spd_shadow =
        ctx->CreateShaderProgramDescription("spd_shadow")
        ->BehavesAsShadowCaster()->DoesNotCull()
        ->WithDepthBias(shadow_rsbp)
		;
	ShaderProgram* sp_shadow = ctx->CreateShaderProgram("sp_shadow", spd_shadow, DefaultRenderPassNamespace::SHADOW_PASS,
        vs_2, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
        fs_2, {},
        {}
	);
    sp_shadow->BindPushConstants<DefaultRenderPassNamespace::ShadowPushData>(
        [](const PushConstantBinder& b, DefaultRenderPassNamespace::ShadowPushData data) {
        b.PushFragment(data);   // слот 0
    });

    render_shadow_inited = true;
}

void DefaultShaderProgramSet::SetTransparentShaderProgram(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (render_transparent_inited) {
        SDL_Log("Transparent render shader programs already initialized.");
        return;
    }

    // VS переиспользуем из main-пасса (общий статик — без дубля GPU-шейдера).
    VertexShaderData vs = GetMainPassVertexShader(ctx);
    FragmentShaderData fs = ctx->CreateFragmentShader("../engine/shaders_code/transparent_pass/surface.hlsl");

    // Блендинг + depth-test без записи (см. BehavesAsTransparentGeometry).
    ShaderProgramDescription* spd_transparent =
        ctx->CreateShaderProgramDescription("spd_transparent")
        ->BehavesAsTransparentGeometry()->DoesNotCull();

    // Без shadow-байндингов: только albedo+normal (2 сэмплера) и буфер света (storage t2).
    ctx->CreateShaderProgram("sp_transparent", spd_transparent, DefaultRenderPassNamespace::TRANSPARENT_PASS,
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER },
        fs, { DEFAULT_LIGHT_BUFFER },
        { TextureSlotRole::Albedo, TextureSlotRole::Normal }
    );

    render_transparent_inited = true;
}

void DefaultShaderProgramSet::SetDebugColliderProgram(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (debug_collider_inited) {
        SDL_Log("Debug collider shader program already initialized.");
        return;
    }

    VertexShaderData vs = ctx->CreateVertexShader(
        "../engine/shaders_code/debug/debug_collider.vert.hlsl",
        { { DEFAULT_VERTEX_BUFFER, &FMT_PosUVNormal, { POSITION } } });
    FragmentShaderData fs = ctx->CreateFragmentShader(
        "../engine/shaders_code/debug/debug_collider.frag.hlsl");

    // Рамки рисуем ПОВЕРХ геометрии (IgnoresDepth — без depth-теста): debug-коллайдеры
    // видны целиком, без вендор-зависимого z-fighting. Топология — line-list через spd
    // (AsLineList); spd сохраняет эту возможность, рамки её используют.
    ShaderProgramDescription* spd =
        ctx->CreateShaderProgramDescription("spd_debug_collider")
        ->DoesNotCull()->IgnoresDepth()->AsLineList();

    ShaderProgram* sp = ctx->CreateShaderProgram("sp_debug_collider", spd,
        DefaultRenderPassNamespace::DEBUG_PASS,
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_CAMERA_BUFFER },
        fs, {},
        {});

    sp->BindPushConstants<DefaultRenderPassNamespace::DebugColliderPushData>(
        [](const PushConstantBinder& b, DefaultRenderPassNamespace::DebugColliderPushData data) {
        b.PushFragment(data);   // fragment slot 0 → b0, space3
    });

    debug_collider_inited = true;
}

void DefaultShaderProgramSet::SetCullingZerosPrograms(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (culling_zeros_inited) {
        SDL_Log("Culling zeros programs already initialized.");
        return;
    }

    ComputeShaderData csd_zeros = ctx->CreateComputeShader("../engine/shaders/culling_clear.comp.spv");

    ctx->CreateComputeShaderProgram("csp_zeros", csd_zeros,
        { DEFAULT_COUNT_BUFFER },   // rw_storage_buffers
        {},                          // ro_storage_buffers
        {}, {}, {},                  // rw_tex, ro_tex, samplers
        DefaultRenderPassNamespace::CULLING_ZEROS_PREPASS);

    culling_zeros_inited = true;
}

void DefaultShaderProgramSet::SetCullingCountPrograms(
    EngineContext* ctx, LightDataModule* ldm)
{
    using namespace DefaultBuffersNames;
    if (culling_count_inited) {
        SDL_Log("Culling count programs already initialized.");
        return;
    }

    ObjectManager* om = ctx->GetObjectManager();
    ComputeShaderData csd = ctx->CreateComputeShader("../engine/shaders/culling_count.comp.spv");
	BatchBuilder* bb = ctx->GetBatchBuilder();

    ComputeShaderProgram* cs_main = ctx->CreateComputeShaderProgram("compute_sp_main", csd,
        { DEFAULT_COUNT_BUFFER },                                                    // rw
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_BATCH_BUFFER,
          DEFAULT_BOUND_SPHERE_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER }, // ro
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_PREPASS);

    cs_main->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingCountUniform>(
        [](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingCountUniform data) {
        data.num_cameras = 1;
        data.cmd_offset = 0;
        binder.Push(0, data);
    });

    ComputeShaderProgram* cs_light = ctx->CreateComputeShaderProgram("compute_sp_light", csd,
        { DEFAULT_COUNT_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_BATCH_BUFFER,
          DEFAULT_BOUND_SPHERE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_PREPASS);

    cs_light->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingCountUniform>(
        [ldm, om, bb](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingCountUniform data) {
        data.num_cameras = ldm->AskNumLightCameras(om, om->GetActiveScene());
        data.cmd_offset = bb->AskNumCommands() * 1;
        binder.Push(0, data);
    });

    culling_count_inited = true;
}

void DefaultShaderProgramSet::SetCullingOffsetPrograms(EngineContext* ctx)
{
    using namespace DefaultBuffersNames;
    if (culling_offset_inited) {
        SDL_Log("Culling offset programs already initialized.");
        return;
    }

    ComputeShaderData csd_offset = ctx->CreateComputeShader("../engine/shaders/culling_offsets.comp.spv");

    ctx->CreateComputeShaderProgram("csp_offsets", csd_offset,
        { DEFAULT_OFFSET_BUFFER },   // rw
        { DEFAULT_COUNT_BUFFER },    // ro
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_OFFSET_PREPASS);

    culling_offset_inited = true;
}

void DefaultShaderProgramSet::SetCullingOutIndirectPrograms(
    EngineContext* ctx, LightDataModule* ldm)
{
    using namespace DefaultBuffersNames;
    if (culling_out_indirect_inited) {
        SDL_Log("Culling out indirect programs already initialized.");
        return;
    }

    ObjectManager* om = ctx->GetObjectManager();
    ComputeShaderData csd_out = ctx->CreateComputeShader("../engine/shaders/culling_out_indirect.comp.spv");
	BatchBuilder* bb = ctx->GetBatchBuilder();

    ComputeShaderProgram* csp_out_main = ctx->CreateComputeShaderProgram("csp_out_indirect", csd_out,
        { DEFAULT_OUT_INDIRECT_BUFFER },                                       // rw
        { DEFAULT_INDIRECT_BUFFER, DEFAULT_COUNT_BUFFER, DEFAULT_OFFSET_BUFFER }, // ro
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_OUT_INDIRECT_PREPASS);

    csp_out_main->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingOutIndirectUniform>(
        [bb](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingOutIndirectUniform data) {
        data.num_commands = bb->AskNumCommands();
        data.num_cameras = 1;
        data.cmd_offset = 0;
        binder.Push(0, data);
    });

    ComputeShaderProgram* csp_out_light = ctx->CreateComputeShaderProgram("csp_out_indirect_lights", csd_out,
        { DEFAULT_OUT_INDIRECT_BUFFER },
        { DEFAULT_INDIRECT_BUFFER, DEFAULT_COUNT_BUFFER, DEFAULT_OFFSET_BUFFER },
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_OUT_INDIRECT_PREPASS);

    csp_out_light->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingOutIndirectUniform>(
        [ldm, om, bb](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingOutIndirectUniform data) {
        data.num_commands = bb->AskNumCommands();
        data.num_cameras = ldm->AskNumLightCameras(om, om->GetActiveScene());
        data.cmd_offset = bb->AskNumCommands() * 1;
        binder.Push(0, data);
    });

    culling_out_indirect_inited = true;
}

void DefaultShaderProgramSet::SetCullingWritePrograms(
    EngineContext* ctx, LightDataModule* ldm)
{
    using namespace DefaultBuffersNames;

	BatchBuilder* bb = ctx->GetBatchBuilder();
    if (culling_write_inited) {
        SDL_Log("Culling write programs already initialized.");
        return;
    }

    ObjectManager* om = ctx->GetObjectManager();
    ComputeShaderData csd_write = ctx->CreateComputeShader("../engine/shaders/culling_write.comp.spv");

    ComputeShaderProgram* csp_main = ctx->CreateComputeShaderProgram("csp_culling_write_main", csd_write,
        { DEFAULT_OFFSET_BUFFER, DEFAULT_OUT_TRANSFORM_BUFFER },                  // rw
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_BATCH_BUFFER,
          DEFAULT_BOUND_SPHERE_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER }, // ro
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_WRITE_PASS);

    csp_main->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingCountUniform>(
        [](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingCountUniform data) {
        data.num_cameras = 1;
        data.cmd_offset = 0;
        binder.Push(0, data);
    });

    ComputeShaderProgram* csp_light = ctx->CreateComputeShaderProgram("csp_culling_write_light", csd_write,
        { DEFAULT_OFFSET_BUFFER, DEFAULT_OUT_TRANSFORM_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_BATCH_BUFFER,
          DEFAULT_BOUND_SPHERE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },
        {}, {}, {},
        DefaultRenderPassNamespace::CULLING_WRITE_PASS);

    csp_light->BindPushConstants<DefaultRenderPassNamespace::ComputeCullingCountUniform>(
        [ldm, om, bb](const PushConstantBinder& binder, DefaultRenderPassNamespace::ComputeCullingCountUniform data) {
        data.num_cameras = ldm->AskNumLightCameras(om, om->GetActiveScene());
        data.cmd_offset = bb->AskNumCommands() * 1;
        binder.Push(0, data);
    });

    culling_write_inited = true;
}

void DefaultShaderProgramSet::SetShadowBlurPrograms(EngineContext* ctx, LightDataModule* ldm)
{
    using namespace DefaultRenderPassNamespace;
    if (shadow_blur_inited) {
        SDL_Log("Shadow blur programs already initialized.");
        return;
    }

    ObjectManager* om = ctx->GetObjectManager();

    ComputeShaderData csd_h = ctx->CreateComputeShader("../engine/shaders_code/comp/shadow_blur_h.comp.hlsl");
    ComputeShaderData csd_v = ctx->CreateComputeShader("../engine/shaders_code/comp/shadow_blur_v.comp.hlsl");

    auto moments_atlas = ctx->GetTextureAtlas(SHADOW_MOMENTS_ARRAY);
    auto blur_temp_atlas = ctx->GetTextureAtlas(SHADOW_MOMENTS_BLUR_TEMP);
    uint32_t LAYER_COUNT = moments_atlas->layers;

    for (uint32_t L = 0; L < LAYER_COUNT; ++L) {
        std::string name_h = "csp_shadow_blur_h_" + std::to_string(L);
        ComputeShaderProgram* csp_h = ctx->CreateComputeShaderProgram(name_h, csd_h,
            {},                                       // rw buffers
            {},                                       // ro buffers
            { { SHADOW_MOMENTS_BLUR_TEMP, 0, 0 } },   // rw textures
            {},                                       // ro storage textures
            { SHADOW_MOMENTS_ARRAY },                 // samplers
            SHADOW_BLUR_PASS);

        csp_h->BindPushConstants<ShadowBlurUniform>(
            [L](const PushConstantBinder& binder, ShadowBlurUniform data) {
            data.layerIndex = L;
            binder.Push(0, data);
        });
        csp_h->BindDispatch<DummyDispatchData>(
            [L, om, blur_temp_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(om, L))
                binder.element_count = { blur_temp_atlas->width, blur_temp_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });

        std::string name_v = "csp_shadow_blur_v_" + std::to_string(L);
        ComputeShaderProgram* csp_v = ctx->CreateComputeShaderProgram(name_v, csd_v,
            {},
            {},
            { { SHADOW_MOMENTS_ARRAY, 0, L } },       // rw textures
            {},
            { SHADOW_MOMENTS_BLUR_TEMP },             // samplers
            SHADOW_BLUR_PASS);

        csp_v->BindDispatch<DummyDispatchData>(
            [L, om, moments_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerDirty(om, L))
                binder.element_count = { moments_atlas->width, moments_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });
    }

    shadow_blur_inited = true;
}

void DefaultShaderProgramSet::SetBloomPrograms(EngineContext* ctx)
{
    using namespace DefaultRenderPassNamespace;
    static bool inited = false;
    if (inited) { SDL_Log("Bloom shader programs already initialized."); return; }

    ComputeShaderData csd_pre  = ctx->CreateComputeShader("../engine/shaders_code/comp/bloom_prefilter.comp.hlsl");
    ComputeShaderData csd_down = ctx->CreateComputeShader("../engine/shaders_code/comp/bloom_down.comp.hlsl");
    ComputeShaderData csd_up   = ctx->CreateComputeShader("../engine/shaders_code/comp/bloom_up.comp.hlsl");
    ComputeShaderData csd_comp = ctx->CreateComputeShader("../engine/shaders_code/comp/bloom_composite.comp.hlsl");

    // Пирамида — BLOOM_LEVELS ОТДЕЛЬНЫХ текстур "bloom_L<i>" (см. _SetDefaultCommonResources):
    // dst-уровень биндится RW-storage, src-уровень — сэмплером. Отдельные текстуры исключают
    // одновременный RW+sampled бинд одной (layout-ошибки валидации на общей мип-цепочке).
    // Размер диспатча — по живому атласу уровня (ресайз меняет width/height внутри атласа).
    auto L = [](uint32_t i) { return "bloom_L" + std::to_string(i); };

    // --- Prefilter (уровень 0): softKnee(scene_hdr) + scene_emission → bloom_L0 (Karis) ---
    {
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_down_0", csd_pre,
            {}, {},
            { { L(0), 0, 0 } },                                            // rw: bloom_L0
            {},
            { std::string("scene_hdr"), std::string("scene_emission") },   // sampler t0/s0, t1/s1
            BLOOM_PASS);
        p->BindPushConstants<BloomParams>([](const PushConstantBinder& b, BloomParams d) {
            d.threshold = 1.2f;   // ПОЛ: диффуз ≤1 не блумит вообще; ярче — только глинт/пересвет
            d.knee      = 0.9f;   // ширина гладкого разгона ВВЕРХ от порога
            d.intensity = 0.1f;   // сила вклада сцены (блик/пересвет); эмиссия идёт в полную силу
            b.Push(0, d);
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L(0));
        p->BindDispatch<DummyDispatchData>([dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Downsample: уровень i-1 → уровень i (1..N-1). Источник — sampler соседнего уровня. ---
    for (uint32_t i = 1; i < BLOOM_LEVELS; ++i) {
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_down_" + std::to_string(i), csd_down,
            {}, {},
            { { L(i), 0, 0 } },   // rw: bloom_L<i>
            {},
            { L(i - 1) },         // combined sampler: предыдущий (вдвое крупнее) уровень
            BLOOM_PASS);
        p->BindPushConstants<BloomParams>([](const PushConstantBinder& b, BloomParams d) {
            d.useKaris = 0u; b.Push(0, d);
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L(i));
        p->BindDispatch<DummyDispatchData>([dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Upsample (tent, аддитивно): уровень i+1 → += уровень i, от мелкого к крупному. ---
    for (int i = (int)BLOOM_LEVELS - 2; i >= 0; --i) {
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_up_" + std::to_string(i), csd_up,
            {}, {},
            { { L((uint32_t)i), 0, 0 } },   // rw: bloom_L<i> (RMW: += размытие → нужен SIMULTANEOUS)
            {},
            { L((uint32_t)i + 1) },         // combined sampler: следующий (вдвое мельче) уровень
            BLOOM_PASS);
        p->BindPushConstants<BloomParams>([](const PushConstantBinder& b, BloomParams d) {
            b.Push(0, d);
        });
        TextureAtlas* dst = ctx->GetTextureAtlas(L((uint32_t)i));
        p->BindDispatch<DummyDispatchData>([dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    // --- Composite: scene_hdr += bloom_L0 * intensity (hue-preserving clip) ---
    {
        auto dst = ctx->GetTextureAtlas(std::string("scene_hdr"));
        ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
            "bloom_composite", csd_comp,
            {}, {},
            { { std::string("scene_hdr"), 0, 0 } },   // rw: scene_hdr (RMW)
            {},
            { L(0) },                                 // combined sampler: bloom_L0
            BLOOM_PASS);
        p->BindPushConstants<BloomParams>([](const PushConstantBinder& b, BloomParams d) {
            d.intensity = 0.5f; b.Push(0, d);   // сила свечения — главная ручка
        });
        p->BindDispatch<DummyDispatchData>([dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    inited = true;
}