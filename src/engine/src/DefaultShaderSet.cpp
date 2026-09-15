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
#include "ResourceTags.h"
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
    PassRegion RegionOfPass(PassManager* pm, uint32_t pass_ordinal, uint8_t slot)
    {
        const PassRegions& regions = pm->AskRegions(slot);
        if (pass_ordinal < regions.per_pass.size()) {
            return regions.per_pass[pass_ordinal];
        }
        return PassRegion{};
    }

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
              camera_buffer, DEFAULT_TRANSFORM_BUFFER },
            {}, {}, {},
            RP::CULLING_PASS, ResTag::DontSave | ResTag::Default);

        sm->CreateComputePushInstruction<RP::CullingPibUniform>(program_name,
            [pm, pass_ordinal, screen_target, invert_span](const PushConstantBinder& binder, RP::CullingPibUniform data) {
            const PassRegion region = RegionOfPass(pm, pass_ordinal, binder.frame);
            data.range_start = region.first_pib;
            data.range_count = region.pib;
            data.num_blocks  = region.command_blocks_count;
            data.cmd_base    = region.cmd_base;
            data.commands    = region.commands;
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
                records = 0;
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
        const UVL_Block empty{};
        b.Push(empty);
    });
    sm->RegisterPushKind("material_params", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput& in) {
        if (!in.draw || !in.draw->params || in.draw->params->empty()) return;
        b.Push(*in.draw->params);
    });
    sm->RegisterPushKind("variant_layout", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput& in) {
        if (!in.draw) return;
        b.Push(in.draw->variant_layout);
    });

    sm->CreatePushInstruction<RP::ShadowPushData>("ShadowCaster", PushStage::Vertex,
        [](const PushConstantBinder& b, RP::ShadowPushData data) { b.Push(data.camera_index); });
    sm->CreatePushInstruction<RP::ShadowPushData>("ShadowCaster", PushStage::Fragment,
        [](const PushConstantBinder& b, RP::ShadowPushData data) { b.Push(data); });
    sm->CreatePushInstruction<RP::DebugColliderPushData>("Wireframe", PushStage::Fragment,
        [](const PushConstantBinder& b, RP::DebugColliderPushData data) { b.Push(data); });
}

void DefaultShaderProgramSet::SetDefaultShaders(EngineContext* ctx)
{
	using namespace DefaultBuffersNames;
	using namespace ShaderBase;
	namespace RP = DefaultRenderPassNamespace;

	SetDefaultPushes(ctx);

	// Отдельная тройка, а не ссылка на main_pass_vs/untextured_surface_fs ниже: фолбэк обязан
	// пережить удаление любого шейдера из редактора. Одинаковый байткод дедуплицируется по хэшу SPIR-V.
	ctx->CreateVertexShader("_fallback_vs",
		"../engine/shaders_code/main_pass/main_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION, UV, NORMAL, TANGENT }, ResTag::DontSave | ResTag::Default | ResTag::System);
	ctx->CreateFragmentShader("_fallback_fs",
		"../engine/shaders_code/main_pass/untextured/surface.hlsl", ResTag::DontSave | ResTag::Default | ResTag::System);
	{
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->DoesNotCull();
		ctx->CreateShaderProgram("_Fallback", spd, RP::MAIN_PASS,
			"_fallback_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"_fallback_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ }, ResTag::DontSave | ResTag::Default | ResTag::System);
		ctx->GetBatchBuilder()->SetFallbackShader("_Fallback");
	}

	ctx->CreateVertexShader("main_pass_vs", "../engine/shaders_code/main_pass/main_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION, UV, NORMAL, TANGENT }, ResTag::DontSave | ResTag::Default);
	ctx->CreateVertexShader("shadow_vs", "../engine/shaders_code/shadow_pass/shadow_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, ResTag::DontSave | ResTag::Default);
	ctx->CreateVertexShader("skybox_vs", "../engine/shaders_code/skybox/skybox.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, ResTag::DontSave | ResTag::Default);
	ctx->CreateVertexShader("debug_collider_vs", "../engine/shaders_code/debug/debug_collider.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, ResTag::DontSave | ResTag::Default);

	const ShaderDefines kVariantDefines = {
		{ "TEXTURE_VARIANTS",    "1" },
		{ "MAX_VARIATIVE_SLOTS", std::to_string(MAX_VARIATIVE_SLOTS) },
		{ "MAX_SLOTS",           std::to_string(MAX_SLOTS) },
		{ "MAX_UVL_BLOCKS",      std::to_string(MAX_UVL_BLOCKS) },
	};
	ctx->CreateFragmentShader("main_surface_fs",        "../engine/shaders_code/main_pass/surface.hlsl", ResTag::DontSave | ResTag::Default, kVariantDefines);
	ctx->CreateFragmentShader("untextured_surface_fs",  "../engine/shaders_code/main_pass/untextured/surface.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateFragmentShader("transparent_surface_fs", "../engine/shaders_code/transparent_pass/surface.hlsl", ResTag::DontSave | ResTag::Default, kVariantDefines);
	ctx->CreateFragmentShader("shadow_fs",              "../engine/shaders_code/shadow_pass/shadow_pass.frag.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateFragmentShader("skybox_fs",              "../engine/shaders_code/skybox/skybox.frag.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateFragmentShader("debug_collider_fs",      "../engine/shaders_code/debug/debug_collider.frag.hlsl", ResTag::DontSave | ResTag::Default);

	ctx->CreateComputeShader("bloom_prefilter_cs", "../engine/shaders_code/comp/bloom_prefilter.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("bloom_down_cs",      "../engine/shaders_code/comp/bloom_down.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("bloom_up_cs",        "../engine/shaders_code/comp/bloom_up.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("bloom_composite_cs", "../engine/shaders_code/comp/bloom_composite.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("ssao_cs",            "../engine/shaders_code/comp/ssao.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("ssao_blur_h_cs",     "../engine/shaders_code/comp/ssao_blur_h.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("ssao_blur_v_cs",     "../engine/shaders_code/comp/ssao_blur_v.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("ao_composite_cs",    "../engine/shaders_code/comp/ao_composite.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("fog_cs",             "../engine/shaders_code/comp/fog.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("culling_clear_cs",   "../engine/shaders_code/comp/culling_clear.comp.hlsl", ResTag::DontSave | ResTag::Default);
	ctx->CreateComputeShader("culling_pib_cs",     "../engine/shaders_code/comp/culling_pib.comp.hlsl", ResTag::DontSave | ResTag::Default);

	{
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry();
		ctx->CreateShaderProgram("Lit", spd, RP::MAIN_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			// Буферы вариантов — во ФРАГМЕНТНОМ списке: main_pass_vs общий и с программами игр,
			// а буфер в вершинном списке обязана была бы биндить КАЖДАЯ из них — иначе
			// «Missing vertex storage buffer binding».
			"main_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo, TextureSlotRole::Normal, TextureSlotRole::ORM, TextureSlotRole::Emissive },
			ResTag::DontSave | ResTag::Default);

		ctx->CreateShaderProgram("LitColor", spd, RP::MAIN_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"untextured_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ }, ResTag::DontSave | ResTag::Default);
	}
	{
		ShaderProgramDescription spd;
		spd.BehavesAsTransparentGeometry();
		ctx->CreateShaderProgram("LitTransparent", spd, RP::TRANSPARENT_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER },
			"transparent_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo, TextureSlotRole::Normal }, ResTag::DontSave | ResTag::Default);
	}
	{
		ShaderProgramDescription spd;
		spd.BehavesAsShadowCaster();
		ctx->CreateShaderProgram("ShadowCaster", spd, RP::SHADOW_PASS,
			"shadow_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"shadow_fs", { }, { }, ResTag::DontSave | ResTag::Default);
	}
	{
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->IgnoresDepth()->AsLineList();
		ctx->CreateShaderProgram("Wireframe", spd, RP::DEBUG_PASS,
			"debug_collider_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER },
			"debug_collider_fs", { }, { }, ResTag::DontSave | ResTag::Default);
	}
	// Сплат ВЫКЛЮЧЕН вместе со своим проходом (см. Engine::Init). Держать sp живой нельзя:
	// её render_pass_name указывал бы на незарегистрированный SPLAT_PASS, а PipeManager на такое
	// ругается на каждой сборке пайплайна. Включать — вместе с SetDefaultSplatPass.
	//
	//	{
	//		ShaderProgramDescription spd;
	//		spd.BehavesAsOpaqueGeometry()->AsPointList();
	//		ctx->CreateShaderProgram("Splat", spd, RP::SPLAT_PASS,
	//			"splat_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER },
	//			"splat_fs", { }, { }, ResTag::DontSave | ResTag::Default);
	//	}
	{
		ShaderProgramDescription spd;
		// z=w в вершиннике даёт глубину РОВНО на клире, поэтому LESS не пройдёт.
		spd.BehavesAsOpaqueGeometry()->ReadsDepthOnly()->WithDepthCompare(SDL_GPU_COMPAREOP_LESS_OR_EQUAL);
		ctx->CreateShaderProgram("Skybox", spd, RP::MAIN_PASS,
			"skybox_vs", { DEFAULT_CAMERA_BUFFER },
			"skybox_fs", { }, { }, ResTag::DontSave | ResTag::Default);
	}

	{
		ctx->CreateVertexShader("ui_vs", "../engine/shaders_code/ui/ui.vert.hlsl",
			POS_UV_NORM_POOL, { POSITION, UV }, ResTag::DontSave | ResTag::Default);
		ctx->CreateFragmentShader("ui_fs", "../engine/shaders_code/ui/ui.frag.hlsl", ResTag::DontSave | ResTag::Default, kVariantDefines);

		ShaderProgramDescription spd;
		spd.BehavesAsUIOverlay();
		ctx->CreateShaderProgram("UI", spd, RP::UI_PASS,
			"ui_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_INSTANCE_BUFFER },
			"ui_fs", { UI_TEXT_RANK_BUFFER, UI_TEXT_INDEX_BUFFER, UI_TEXT_BUFFER, UI_FONT_UVL_BUFFER,
			           DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo }, ResTag::DontSave | ResTag::Default);
	}

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

    ComputeShaderProgram* csp_clear = ctx->CreateComputeShaderProgram("csp_culling_clear", "culling_clear_cs",
        { DEFAULT_INDIRECT_BUFFER },
        {}, {}, {}, {},
        RP::CULLING_PASS, ResTag::DontSave | ResTag::Default);
    sm->CreateComputePushInstruction<RP::CullingClearUniform>("csp_culling_clear",
        [pm](const PushConstantBinder& binder, RP::CullingClearUniform data) {
        data.total_slots = pm->AskRegions(binder.frame).total_commands;
        binder.Push(data);
    });
    sm->CreateDispatchInstruction<RP::DummyDispatchData>("csp_culling_clear",
        [pm](DispatchSizeBinder& binder, RP::DummyDispatchData) {
        binder.element_count = { pm->AskRegions(binder.frame).total_commands, 1, 1 };
    });

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
            {},
            {},
            { { SHADOW_MOMENTS_BLUR_TEMP, 0, 0 } },
            {},
            { SHADOW_MOMENTS_ARRAY },
            SHADOW_BLUR_PASS, ResTag::DontSave | ResTag::Default);

        sm->CreateComputePushInstruction<ShadowBlurUniform>(name_h,
            [L](const PushConstantBinder& binder, ShadowBlurUniform data) {
            data.layerIndex = L;
            binder.Push(data);
        });
        sm->CreateDispatchInstruction<DummyDispatchData>(name_h,
            [L, blur_temp_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerUsed(binder.frame, L))
                binder.element_count = { blur_temp_atlas->width, blur_temp_atlas->height, 1 };
            else
                binder.element_count = { 0, 0, 0 };
        });

        std::string name_v = "csp_shadow_blur_v_" + std::to_string(L);
        ComputeShaderProgram* csp_v = ctx->CreateComputeShaderProgram(name_v, "shadow_blur_v_cs",
            {},
            {},
            { { SHADOW_MOMENTS_ARRAY, 0, L } },
            {},
            { SHADOW_MOMENTS_BLUR_TEMP },
            SHADOW_BLUR_PASS, ResTag::DontSave | ResTag::Default);

        sm->CreateDispatchInstruction<DummyDispatchData>(name_v,
            [L, moments_atlas, ldm](DispatchSizeBinder& binder, DummyDispatchData) {
            if (ldm->IsShadowLayerUsed(binder.frame, L))
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

    ctx->CreateComputeShaderProgram("ssao", "ssao_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEXTURE, 0, 0 } },
        {},
        { std::string("__main_depth") },
        AO_PASS, ResTag::DontSave | ResTag::Default);

    ctx->CreateComputeShaderProgram("ssao_blur_h", "ssao_blur_h_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEMP, 0, 0 } },
        {},
        { SSAO_TEXTURE, std::string("__main_depth") },
        AO_PASS, ResTag::DontSave | ResTag::Default);

    ctx->CreateComputeShaderProgram("ssao_blur_v", "ssao_blur_v_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { SSAO_TEXTURE, 0, 0 } },
        {},
        { SSAO_TEMP, std::string("__main_depth") },
        AO_PASS, ResTag::DontSave | ResTag::Default);

    ctx->CreateComputeShaderProgram("ao_composite", "ao_composite_cs",
        {}, {},
        { { std::string("scene_hdr"), 0, 0 } },
        {},
        { SCENE_AMBIENT, SSAO_TEXTURE },
        AO_PASS, ResTag::DontSave | ResTag::Default);

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

    ctx->CreateComputeShaderProgram("fog", "fog_cs",
        {}, { DEFAULT_CAMERA_BUFFER },
        { { std::string("scene_hdr"), 0, 0 } },
        {},
        { std::string("__main_depth") },
        FOG_PASS, ResTag::DontSave | ResTag::Default);

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

    auto L = [](uint32_t i) { return "__bloom_L" + std::to_string(i); };

    ComputeShaderProgram* p = ctx->CreateComputeShaderProgram(
        "bloom_down_0", "bloom_prefilter_cs",
        {}, {},
        { { L(0), 0, 0 } },
        {},
        { std::string("scene_hdr"), std::string("scene_emission") },
        BLOOM_PASS, ResTag::DontSave | ResTag::Default);
    sm->CreateComputePushInstruction<BloomState>("bloom_down_0",[](const PushConstantBinder& b, BloomState st) {
        BloomParams d{};
        d.threshold = st.threshold;
        d.knee      = st.knee;
        d.intensity = st.scene_contribution;
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
            { { L(i), 0, 0 } },
            {},
            { L(i - 1) },
            BLOOM_PASS, ResTag::DontSave | ResTag::Default);
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
            { { .texture_atlas = L((uint32_t)i), .need_simultaneous = true } },
            {},
            { L((uint32_t)i + 1) },
            BLOOM_PASS, ResTag::DontSave | ResTag::Default);
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
            { { std::string("scene_hdr"), 0, 0 } },
            {},
            { L(0) },
            BLOOM_PASS, ResTag::DontSave | ResTag::Default);
        sm->CreateComputePushInstruction<BloomState>("bloom_composite",[](const PushConstantBinder& b, BloomState st) {
            BloomParams d{};
            d.intensity = st.glow_intensity;
            b.Push(d);
        });
        sm->CreateDispatchInstruction<DummyDispatchData>("bloom_composite",[dst](DispatchSizeBinder& b, DummyDispatchData) {
            b.element_count = { dst->width, dst->height, 1 };
        });
    }

    inited = true;
}
