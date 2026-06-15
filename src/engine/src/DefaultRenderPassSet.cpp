#include "DefaultRenderPassSet.h"
#include "TextureManager.h"
#include "RenderManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "ObjectManager.h"
#include "TransformDataModule.h"
#include "LightDataModule.h"
#include "IndirectDataModule.h"
#include "BatchBuilder.h"
#include "EngineContext.h"

namespace DefaultRenderPassNamespace
{
    static TextureAtlas* shadow_moments_array = nullptr;
    static TextureAtlas* shadow_depth_flat_array = nullptr;


    bool shadow_pass_inited = false;
	bool main_pass_inited = false;

    static SDL_GPUTexture* main_color_half = nullptr; // левая половина — сцена
    static SDL_GPUTexture* debug_color_half = nullptr; // правая половина — debug-затычка
    static SDL_GPUTexture* split_depth_half = nullptr; // общий depth обеих половин
    static Uint32 split_half_w = 0;                    // нужен copy-степу для смещения правой половины
    static Uint32 split_full_h = 0;
}



void DefaultRenderPassNamespace::SetDefaultShadowPCFRenderPass(EngineContext* ctx)
{
    if (shadow_pass_inited) {
        SDL_Log("Default shadow render pass is already initialized.");
        return;
    }
	TextureManager* tm = ctx->GetTextureManager();
	PassManager* pm = ctx->GetRenderManager();
	BufferManager* bm = ctx->GetBufferManager();
	ObjectManager* om = ctx->GetObjectManager();

    auto shadow_sampler = tm->GetSampler(DefaultSamplersNames::DEFAULT_SHADOW_SAMPLER);

    shadow_depth_flat_array = tm->CreateTextureAtlas(SHADOW_DEPTH_FLAT_ARRAY, TexturePresets::GetCreateInfo(TexturePreset::Depth_FlatArray1024_8Layers), shadow_sampler);
    TextureAtlas* shadow_temp = tm->CreateTextureAtlas("shadow_depth_single_temp", TexturePresets::GetCreateInfo(TexturePreset::TempDepth1024), shadow_sampler);

    RenderPassTexturesInfo shadow_rptd{};
    shadow_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, shadow_depth_flat_array->format);

    auto shadowPass = pm->CreateRenderPass(
        SHADOW_PASS,
        [pm, bm, om, tm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        Uint32 cameraIndex = 0;
        Uint32 sphereLayer = 0;

        auto flat_array = tm->GetTextureAtlas(SHADOW_DEPTH_FLAT_ARRAY);

        om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SpotLightComponent& light, ShadowCasterComponent& sc) {
            //SDL_Log("Shadow pass: cameraIndex=%u, spotLayer=%u", cameraIndex, spotLayer);

            if (light.needsUpdate) {
				ShadowPushData push_data{};
				push_data.cameraIndex = cameraIndex;
                push_data.max_range = light.light_data.GetMaxDistance();
                SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));
                pm->RenderPassStandardBody(cb, &rp, bm, 0, &push_data);

                auto cp = SDL_BeginGPUCopyPass(cb);
                SDL_GPUTextureLocation src = {
                    .texture = rp.renderPassTexsData.depthTargetInfo.texture,
                    .layer = 0
                };
                SDL_GPUTextureLocation dst = {
                    .texture = flat_array->texture_binding.texture,
                    .layer = cameraIndex
                };
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, flat_array->width, flat_array->height, 1, false);
                SDL_EndGPUCopyPass(cp);
            };
            cameraIndex++;
        }
        );

        om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SphereLightComponent& light, ShadowCasterComponent& sc) {
            for (int face = 0; face < 6; ++face) {
                if (light.needsUpdate) {
                    ShadowPushData push_data{};
					push_data.cameraIndex = cameraIndex;
					push_data.max_range = light.light_data.GetMaxDistance();
                    SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));

                    pm->RenderPassStandardBody(cb, &rp, bm, 0, &push_data);

                    auto cp = SDL_BeginGPUCopyPass(cb);
                    SDL_GPUTextureLocation src = {
                        .texture = rp.renderPassTexsData.depthTargetInfo.texture
                    };
                    SDL_GPUTextureLocation dst = {
                        .texture = flat_array->texture_binding.texture,
                        .layer = cameraIndex
                    };
                    SDL_CopyGPUTextureToTexture(cp, &src, &dst, flat_array->width, flat_array->height, 1, false);
                    SDL_EndGPUCopyPass(cp);
                }
                cameraIndex++;
            }

            sphereLayer++;
        });
    },
        std::move(shadow_rptd),
        10
    );
    shadowPass->renderPassTexsData.SetDepthTexture(
        shadow_temp->texture_binding.texture
    );

    shadow_pass_inited = true;
}

void DefaultRenderPassNamespace::SetDefaultMainRenderPass(EngineContext* ctx)
{
    if (main_pass_inited) {
        SDL_Log("Default main render pass is already initialized.");
        return;
    }
    if (!shadow_pass_inited) {
        SDL_Log("Default shadow render pass must be initialized before the default main render pass.");
        return;
    }
    TextureManager* tm = ctx->GetTextureManager();
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    auto tci = TexturePresets::GetCreateInfo(TexturePreset::SingleDepth2048);
    tci.width = 800;
    tci.height = 600;
    tm->main_pass_depth_texture = tm->CreateGPU_Texture(tci);

    RenderPassTexturesInfo main_rptd{};
    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.1f,0.1f,0.14f,1.0f }, SDL_GPU_TEXTUREFORMAT_INVALID);
    main_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, tci.format);

    auto mainPass = pm->CreateRenderPass(
        MAIN_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(main_rptd),
        20
    );


    mainPass->global_texture_bindings = { shadow_depth_flat_array->texture_binding };
    mainPass->renderPassTexsData.SetDepthTexture(tm->main_pass_depth_texture);

    main_pass_inited = true;
}

void DefaultRenderPassNamespace::SetDefaultMainRenderPass(EngineContext* ctx,
    SDL_GPUDevice* dev, SDL_Window* win)
{
    if (main_pass_inited) {
        SDL_Log("Default main render pass is already initialized.");
        return;
    }
    if (!shadow_pass_inited) {
        SDL_Log("Default shadow render pass must be initialized before the default main render pass.");
        return;
    }
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();
	TextureManager* tm = ctx->GetTextureManager();

    const SDL_GPUTextureFormat sc_format = SDL_GetGPUSwapchainTextureFormat(dev, win);

    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(win, &win_w, &win_h);
    split_half_w = (Uint32)win_w / 2;
    split_full_h = (Uint32)win_h;

    // --- offscreen-таргеты, один раз ---
    SDL_GPUTextureCreateInfo color_ci{};
    color_ci.type = SDL_GPU_TEXTURETYPE_2D;
    color_ci.format = sc_format;                          // == swapchain, чтобы copy шёл без конвертации
    color_ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;  // для copy-source отдельный флаг не нужен
    color_ci.width = split_half_w;
    color_ci.height = split_full_h;
    color_ci.layer_count_or_depth = 1;
    color_ci.num_levels = 1;
    color_ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    main_color_half = tm->CreateGPU_Texture(color_ci);
    debug_color_half = tm->CreateGPU_Texture(color_ci);

    auto depth_ci = TexturePresets::GetCreateInfo(TexturePreset::SingleDepth2048);
    depth_ci.width = split_half_w;
    depth_ci.height = split_full_h;
    split_depth_half = tm->CreateGPU_Texture(depth_ci);

    // --- SCENE_PASS: бывший MAIN_PASS, рисует сцену в левую половину ---
    RenderPassTexturesInfo scene_rptd{};
    scene_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.1f,0.1f,0.14f,1.0f }, sc_format);
    scene_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, depth_ci.format);

    auto scenePass = pm->CreateRenderPass(
        "SCENE_PASS",
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(scene_rptd),
        20
    );
    scenePass->global_texture_bindings = { shadow_depth_flat_array->texture_binding };
    scenePass->renderPassTexsData.SetColorTexture(main_color_half);
    scenePass->renderPassTexsData.SetDepthTexture(split_depth_half);

    // --- DEBUG_PASS: затычка с такой же color+depth-структурой, рисует в правую половину ---
    RenderPassTexturesInfo debug_rptd{};
    debug_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.14f,0.1f,0.1f,1.0f }, sc_format);
    debug_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, depth_ci.format);

    auto debugPass = pm->CreateRenderPass(
        "DEBUG_PASS",
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        // ЗАТЫЧКА: пока та же сцена; сюда позже воткнёшь debug-пайплайн
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(debug_rptd),
        25
    );
    debugPass->global_texture_bindings = { shadow_depth_flat_array->texture_binding };
    debugPass->renderPassTexsData.SetColorTexture(debug_color_half);
    debugPass->renderPassTexsData.SetDepthTexture(split_depth_half);

    // --- MAIN_PASS: теперь copy-step в swapchain ---
    RenderPassTexturesInfo main_rptd{};
    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, SDL_GPU_TEXTUREFORMAT_INVALID);
    // depth не нужен — это copy, а не render pass

    auto mainPass = pm->CreateRenderPass(
        MAIN_PASS,
        [](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        // swapchain поставлен в RenderFunc через SetColorTexture(tex)
        SDL_GPUTexture* swap = rp.renderPassTexsData.colorTargetInfo.texture; // (или [0].texture, если массив)
        if (!swap || !main_color_half || !debug_color_half) return;

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);

        SDL_GPUTextureLocation src_left{};  src_left.texture = main_color_half;
        SDL_GPUTextureLocation dst_left{};   dst_left.texture = swap; dst_left.x = 0;             dst_left.y = 0;
        SDL_CopyGPUTextureToTexture(cp, &src_left, &dst_left, split_half_w, split_full_h, 1, false);

        SDL_GPUTextureLocation src_right{}; src_right.texture = debug_color_half;
        SDL_GPUTextureLocation dst_right{};  dst_right.texture = swap; dst_right.x = split_half_w; dst_right.y = 0;
        SDL_CopyGPUTextureToTexture(cp, &src_right, &dst_right, split_half_w, split_full_h, 1, false);

        SDL_EndGPUCopyPass(cp);
    },
        std::move(main_rptd),
        30
    );

    main_pass_inited = true;
}


void DefaultRenderPassNamespace::SetDefaultShadowVSMRenderPass(EngineContext* ctx)
{
    if (shadow_pass_inited) {
        SDL_Log("Default shadow render pass is already initialized.");
        return;
    }
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();
	TextureManager* tm = ctx->GetTextureManager();
	ObjectManager* om = ctx->GetObjectManager();
	BatchBuilder* bb = ctx->GetBatchBuilder();

    auto shadow_sampler = tm->GetSampler(DefaultSamplersNames::VSM_SAMPLER);
    auto vsm_sampler = tm->GetSampler(DefaultSamplersNames::VSM_SAMPLER);

    shadow_moments_array = tm->CreateTextureAtlas(SHADOW_MOMENTS_ARRAY, TexturePresets::GetCreateInfo(TexturePreset::ShadowRG32_FlatArray1024_8Layers), vsm_sampler);
    TextureAtlas* shadow_depth_tex = tm->CreateTextureAtlas("shadow_depth_single_temp", TexturePresets::GetCreateInfo(TexturePreset::TempDepth1024), shadow_sampler);
    TextureAtlas* shadow_moments_temp = tm->CreateTextureAtlas(SHADOW_MOMENTS_BLUR_TEMP, TexturePresets::GetCreateInfo(TexturePreset::TempShadowRG32_1024), vsm_sampler);

    RenderPassTexturesInfo shadow_rptd{};
    shadow_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, shadow_depth_tex->format);
    shadow_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 1.0, 1.0, 1.0, 1.0 }, shadow_moments_array->format);

    auto shadowPass = pm->CreateRenderPass(
        SHADOW_PASS,
        [pm, bm, om, tm, bb](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        Uint32 cameraIndex = 0;      // общий для всех типов света
        Uint32 sphereLayer = 0;      // только для sphere

        auto flat_array = tm->GetTextureAtlas(SHADOW_MOMENTS_ARRAY);
        uint32_t commands_byte_offset = bb->AskNumCommands() * sizeof(SDL_GPUIndexedIndirectDrawCommand);
        om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SpotLightComponent& light, ShadowCasterComponent& sc) {
            //SDL_Log("Shadow pass: cameraIndex=%u, spotLayer=%u", cameraIndex, spotLayer);
            uint32_t byte_offset = (1 + cameraIndex) * commands_byte_offset;
            if (light.needsUpdate) {
                SDL_PushGPUVertexUniformData(cb, 0, &cameraIndex, sizeof(Uint32));
                rp.renderPassTexsData.colorTargetInfo.layer_or_depth_plane = cameraIndex;
                pm->RenderPassStandardBody(cb, &rp, bm, 0, &cameraIndex);
            };
            cameraIndex++;
        }
        );

        om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SphereLightComponent& light, ShadowCasterComponent& sc) {
            for (int face = 0; face < 6; ++face) {
                uint32_t byte_offset = (1 + cameraIndex) * commands_byte_offset;
                if (light.needsUpdate) {
                    SDL_PushGPUVertexUniformData(cb, 0, &cameraIndex, sizeof(Uint32));

                    rp.renderPassTexsData.colorTargetInfo.layer_or_depth_plane = cameraIndex;
                    pm->RenderPassStandardBody(cb, &rp, bm, 0, &cameraIndex);
                }
                cameraIndex++;
            }

            sphereLayer++;
        });
    },
        std::move(shadow_rptd),
        10
    );
    shadowPass->renderPassTexsData.SetColorTexture(
        shadow_moments_array->texture_binding.texture
    );
    shadowPass->renderPassTexsData.SetDepthTexture(
        shadow_depth_tex->texture_binding.texture
    );

    shadow_pass_inited = true;
}

void DefaultRenderPassNamespace::SetDefaultShadowBlurPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    pm->CreateComputePass(
        SHADOW_BLUR_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
    {
        ShadowBlurUniform push_data = {};
        DummyDispatchData dispatch_data = {};
        pm->ComputePassStandardBody(cb, &cp, bm, &push_data, &dispatch_data, pass_frame);
    },
        11
    );
}

void DefaultRenderPassNamespace::SetDefaultCullingComputeZerosPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    auto compute_zeros_pass = pm->CreateComputePrepass(
        CULLING_ZEROS_PREPASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame) 
        {
            pm->ComputePassStandardBody(cb, &cp, bm, nullptr, nullptr, pass_frame);
        },
    0
    );
}

void DefaultRenderPassNamespace::SetDefaultCullingComputeCountPass(EngineContext* ctx, TransformDataModule* tdm, LightDataModule* ldm, IndirectDataModule* idm)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();
    ObjectManager* om = ctx->GetObjectManager();

    auto compute_pass = pm->CreateComputePrepass(
        CULLING_PREPASS,
        [pm, bm, om, tdm, ldm, idm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
        {
            ComputeCullingCountUniform data;
            data.num_instances = tdm->AskNumTransform(om, om->GetActiveScene());
            data.num_commands = idm->AskNumCommands(pm);

		    pm->ComputePassStandardBody(cb, &cp, bm, &data, nullptr, pass_frame);
        },
        10
		);
}

void DefaultRenderPassNamespace::SetDefaultCullingOffstPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    auto compute_pass = pm->CreateComputePrepass(CULLING_OFFSET_PREPASS,
        [bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame) {
            pm->ComputePassStandardBody(cb, &cp, bm, nullptr, nullptr, pass_frame);
        },
        20
    );
}

void DefaultRenderPassNamespace::SetDefaultCullingOutIndirectPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    pm->CreateComputePrepass(CULLING_OUT_INDIRECT_PREPASS,
        [bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame) {
            ComputeCullingOutIndirectUniform data;
            pm->ComputePassStandardBody(cb, &cp, bm, &data, nullptr, pass_frame);
        },
        30
    );
}

void DefaultRenderPassNamespace::SetDefaultCullingOutTransformPass(EngineContext* ctx, TransformDataModule* tdm, LightDataModule* ldm, IndirectDataModule* idm)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();
	ObjectManager* om = ctx->GetObjectManager();

    pm->CreateComputePass(
        CULLING_WRITE_PASS,
        [pm, bm, om, tdm, ldm, idm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
        {
            ComputeCullingCountUniform data;
            data.num_instances = tdm->AskNumTransform(om, om->GetActiveScene());
            data.num_commands = idm->AskNumCommands(pm);

            pm->ComputePassStandardBody(cb, &cp, bm, &data, nullptr, pass_frame);
        },
        0
    );
}

