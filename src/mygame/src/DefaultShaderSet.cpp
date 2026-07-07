#include "PCH.h"
#include "DefaultShaderSet.h"
#include "LightDataModule.h"
#include "TransformDataModule.h"
#include "DefaultRenderPassSet.h"
#include "PositionStructure.h"
#include "EngineContext.h"
#include "RenderManager.h"   // PassManager + RenderPassStep (счёт теневых команд для scatter)

namespace {
    // shadow_pib = сумма инстансов теневых команд = ГРАНИЦА PIB между теневыми [0, shadow_pib) и
    // остальными [shadow_pib, N). Она же first_instance первой не-теневой команды (shadow первый).
    // Идёт под BatchTreeMutex (push в ExecutePrepassesSteps).
    uint32_t CountShadowInstances(PassManager* pm)
    {
        uint32_t s = 0;
        if (RenderPassStep* rp = pm->GetRenderPassStep(DefaultRenderPassNamespace::SHADOW_PASS))
            for (auto& [_, sb] : rp->shader_batches)
                for (auto& [_, ab] : sb.atlases_batches)
                    for (auto& [_, tb] : ab.texture_batches)
                        for (auto& [_, mb] : tb.model_batches)
                            s += mb.instanceCount;
        return s;
    }
}

namespace DefaultShaderProgramSet
{
    // render
    bool render_main_inited = false;
    bool render_shadow_inited = false;
    bool render_transparent_inited = false;
    bool debug_collider_inited = false;

    // compute
    bool culling_pib_inited = false;

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
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
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
        vs_2, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
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
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER },
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
        vs, { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER },
        fs, {},
        {});

    sp->BindPushConstants<DefaultRenderPassNamespace::DebugColliderPushData>(
        [](const PushConstantBinder& b, DefaultRenderPassNamespace::DebugColliderPushData data) {
        b.PushFragment(data);   // fragment slot 0 → b0, space3
    });

    debug_collider_inited = true;
}

void DefaultShaderProgramSet::SetCullingPibPrograms(EngineContext* ctx, LightDataModule* ldm)
{
    using namespace DefaultBuffersNames;
    if (culling_pib_inited) {
        SDL_Log("Culling PIB program already initialized.");
        return;
    }

    namespace RP = DefaultRenderPassNamespace;
    ObjectManager* om = ctx->GetObjectManager();
    PassManager*   pm = ctx->GetPassManager();
    BatchBuilder*  bb = ctx->GetBatchBuilder();

    // (1) CLEAR — обнуляет num_instances ВСЕХ (камера,команда) перед scatter. Создаётся ПЕРВОЙ →
    // shader_batch[0] в CULLING_PASS, SDL барьерит между compute-пассами → scatter видит нули.
    ComputeShaderData csd_clear = ctx->CreateComputeShader("../engine/shaders_code/comp/culling_clear.comp.hlsl");
    ComputeShaderProgram* csp_clear = ctx->CreateComputeShaderProgram("csp_culling_clear", csd_clear,
        { DEFAULT_INDIRECT_BUFFER },   // rw (u0)
        {}, {}, {}, {},
        RP::CULLING_PASS);
    csp_clear->BindPushConstants<RP::CullingClearUniform>(
        [ldm, om, bb](const PushConstantBinder& binder, RP::CullingClearUniform data) {
        data.total_slots = (1 + ldm->AskNumLightCameras(om, om->GetActiveScene())) * bb->AskNumCommands();
        binder.Push(0, data);
    });
    csp_clear->BindDispatch<RP::DummyDispatchData>(
        [ldm, om, bb](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        binder.element_count = { (1 + ldm->AskNumLightCameras(om, om->GetActiveScene())) * bb->AskNumCommands(), 1, 1 };
    });

    // Общий шейдер scatter'а для всех групп камер (разные программы = разные камерные буферы).
    ComputeShaderData csd = ctx->CreateComputeShader("../engine/shaders_code/comp/culling_pib.comp.hlsl");

    // (2) ИГРОК — камера игрока (блок 0, num_blocks=1), main-записи [shadow_pib, N). Cameras=CAMERA_BUFFER (t3).
    ComputeShaderProgram* csp_player = ctx->CreateComputeShaderProgram("csp_cull_player", csd,
        { DEFAULT_OUT_PIB_BUFFER, DEFAULT_INDIRECT_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_CMD_BUFFER, DEFAULT_BOUND_SPHERE_BUFFER,
          DEFAULT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },   // ro t0..t4 (Cameras = игрок)
        {}, {}, {},
        RP::CULLING_PASS);
    csp_player->BindPushConstants<RP::CullingPibUniform>(
        [pm, bb](const PushConstantBinder& binder, RP::CullingPibUniform data) {
        uint32_t sp = CountShadowInstances(pm);
        uint32_t N  = bb->AskNumInstances();
        data.range_start = sp;
        data.range_count = (N > sp) ? (N - sp) : 0u;
        data.block_base = 0u;
        data.num_blocks = 1u;
        data.block_stride = N;
        data.total_commands = bb->AskNumCommands();
        binder.Push(0, data);
    });
    csp_player->BindDispatch<RP::DummyDispatchData>(
        [pm, bb](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        uint32_t sp = CountShadowInstances(pm);
        uint32_t N  = bb->AskNumInstances();
        binder.element_count = { (N > sp) ? (N - sp) : 0u, 1, 1 };
    });

    // (3) СВЕТ — световые камеры (блоки 1..L, num_blocks=L), теневые записи [0, shadow_pib).
    // Cameras=LIGHT_CAMERA_BUFFER (t3). При L=0 диспатч 0 → пропуск.
    ComputeShaderProgram* csp_light = ctx->CreateComputeShaderProgram("csp_cull_light", csd,
        { DEFAULT_OUT_PIB_BUFFER, DEFAULT_INDIRECT_BUFFER },
        { DEFAULT_POSITION_INDEX_BUFFER, DEFAULT_ENTITY_TO_CMD_BUFFER, DEFAULT_BOUND_SPHERE_BUFFER,
          DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_TRANSFORM_BUFFER },   // ro t0..t4 (Cameras = свет)
        {}, {}, {},
        RP::CULLING_PASS);
    csp_light->BindPushConstants<RP::CullingPibUniform>(
        [ldm, om, pm, bb](const PushConstantBinder& binder, RP::CullingPibUniform data) {
        uint32_t L  = ldm->AskNumLightCameras(om, om->GetActiveScene());
        uint32_t sp = CountShadowInstances(pm);
        data.range_start = 0u;
        data.range_count = (L > 0) ? sp : 0u;
        data.block_base = 1u;
        data.num_blocks = L;
        data.block_stride = bb->AskNumInstances();
        data.total_commands = bb->AskNumCommands();
        binder.Push(0, data);
    });
    csp_light->BindDispatch<RP::DummyDispatchData>(
        [ldm, om, pm](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        uint32_t L  = ldm->AskNumLightCameras(om, om->GetActiveScene());
        binder.element_count = { (L > 0) ? CountShadowInstances(pm) : 0u, 1, 1 };
    });

    culling_pib_inited = true;
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