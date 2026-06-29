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
#include "TextureLoader.h"

namespace DefaultRenderPassNamespace
{
    static TextureAtlas* shadow_moments_array = nullptr;
    static TextureAtlas* shadow_depth_flat_array = nullptr;

    // "Псевдокласс" PassSystem: общие ресурсы дефолтного набора проходов. Создаются один раз
    // в _SetDefaultCommonResources, дальше их ПОТРЕБЛЯЮТ конкретные Set*Pass — свободные функции
    // ниже концептуально "методы" этого псевдокласса, разделяющие данное состояние. Так формат и
    // depth-таргет живут в одном месте, а не переоткрываются в каждом проходе.
    struct PassSystemState {
        SharedDepthTarget*   main_depth = nullptr;                                 // depth MAIN/TRANSPARENT/DEBUG
        SDL_GPUTextureFormat main_depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;    // единый формат depth набора
        bool                 common_inited = false;
    };
    static PassSystemState g_pass_system;

    bool shadow_pass_inited = false;
	bool main_pass_inited = false;

    static SDL_GPUTexture* main_color_half = nullptr; // левая половина — сцена
    static SDL_GPUTexture* debug_color_half = nullptr; // правая половина — debug-затычка
    static SDL_GPUTexture* split_depth_half = nullptr; // общий depth обеих половин
    static Uint32 split_half_w = 0;                    // нужен copy-степу для смещения правой половины
    static Uint32 split_full_h = 0;

    static TextureAtlas* default_env_atlas = nullptr;

    // Env-окружение для отражений металла. texture_binding куба → в global_texture_bindings пасса
    // (слот 1, после тени), шейдер сэмплит через sampleEnv. Файла нет — фолбэк 1×1×6 куб тоном сцены.
    static SDL_GPUTextureSamplerBinding GetDefaultEnvBinding(EngineContext* ctx)
    {
        if (!default_env_atlas) {
            TextureManager* tm = ctx->GetTextureManager();
            auto env_sampler = tm->GetSampler(DefaultSamplersNames::ENV_SAMPLER);

            // Cube-атлас создаётся ОТДЕЛЬНО (его характер задаёт tci-пресет EnvCube), а нарезку
            // 4×3 креста на 6 граней и заливку делает ctx->CreateCubeMapTexture. faceSize пресета —
            // единственный источник истины о разрешении env-куба.
            TextureAtlas* cube = tm->CreateTextureAtlas("env_skybox", TexturePresets::EnvCube(200), env_sampler);

            // Путь — ассет игры (CWD = src/game); временно здесь, позже окружение задаёт игра.
            if (cube && ctx->CreateCubeMapTexture("env_skybox", "env_skybox", "textures/assets/skybox.png"))
                default_env_atlas = cube;

            if (!default_env_atlas) {
                // Фолбэк: вырожденный 1×1×6 куб тоном сцены (тот же пресет с faceSize=1).
                default_env_atlas = tm->CreateTextureAtlas("env_fallback", TexturePresets::EnvCube(1), env_sampler);
                for (int f = 0; f < 6; ++f) {
                    std::vector<std::byte> px(4);
                    px[0] = std::byte{ 36 }; px[1] = std::byte{ 26 }; px[2] = std::byte{ 26 }; px[3] = std::byte{ 255 }; // BGRA ≈ тон сцены
                    tm->CreateTexture("env_fallback_f" + std::to_string(f), default_env_atlas, 1, 1, std::move(px));
                }
            }
        }
        return default_env_atlas->texture_binding;
    }
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
    auto shadow_tci = TexturePresets::GetCreateInfo(TexturePreset::Depth_FlatArray1024_8Layers);
    uint32_t max_layers = shadow_tci.layer_count_or_depth;

    shadow_depth_flat_array = tm->CreateTextureAtlas(SHADOW_DEPTH_FLAT_ARRAY, shadow_tci, shadow_sampler);
    TextureAtlas* shadow_temp = tm->CreateTextureAtlas("shadow_depth_single_temp", TexturePresets::GetCreateInfo(TexturePreset::TempDepth1024), shadow_sampler);

    RenderPassTexturesInfo shadow_rptd{};
    shadow_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, shadow_temp->format);

    auto shadowPass = pm->CreateRenderPass(
        SHADOW_PASS,
        [pm, bm, om, tm, max_layers](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        uint32_t camera_index = 0;
        uint32_t sphere_layer = 0;

        auto flat_array = tm->GetTextureAtlas(SHADOW_DEPTH_FLAT_ARRAY);

        om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SpotLightComponent& light, ShadowCasterComponent& sc) {
            //SDL_Log("Shadow pass: camera_index=%u, spotLayer=%u", camera_index, spotLayer);
            if (camera_index >= max_layers) {
                return;
            }
            if (light.needsUpdate) {
				ShadowPushData push_data{};
				push_data.camera_index = camera_index;
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
                    .layer = camera_index
                };
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, flat_array->width, flat_array->height, 1, false);
                SDL_EndGPUCopyPass(cp);
            };
            camera_index++;
        }
        );

        om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SphereLightComponent& light, ShadowCasterComponent& sc) {
            for (int face = 0; face < 6; ++face) {
                if (camera_index >= max_layers) {
                    return;
                }
                if (light.needsUpdate) {
                    ShadowPushData push_data{};
					push_data.camera_index = camera_index;
					push_data.max_range = light.light_data.GetMaxDistance();
                    SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));

                    pm->RenderPassStandardBody(cb, &rp, bm, 0, &push_data);

                    auto cp = SDL_BeginGPUCopyPass(cb);
                    SDL_GPUTextureLocation src = {
                        .texture = rp.renderPassTexsData.depthTargetInfo.texture
                    };
                    SDL_GPUTextureLocation dst = {
                        .texture = flat_array->texture_binding.texture,
                        .layer = camera_index
                    };
                    SDL_CopyGPUTextureToTexture(cp, &src, &dst, flat_array->width, flat_array->height, 1, false);
                    SDL_EndGPUCopyPass(cp);
                }
                camera_index++;
            }
            sphere_layer++;
        });

        // Directional: тот же порядок spot→sphere→direct, что в StoreLightCameras —
        // camera_index обязан совпадать. is_ortho=1 → shadow frag пишет линейную осевую глубину.
        om->ForEach<DirectLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](DirectLightComponent& light, ShadowCasterComponent& sc) {
            for (int c = 0; c < light.light_data.cascade_count; ++c) {
                if (camera_index >= max_layers) {
                    return;
                }
                if (light.needsUpdate) {
                    ShadowPushData push_data{};
                    push_data.camera_index = camera_index;
                    push_data.max_range = light.light_data.CascadeFar(c);   // per-cascade far
                    push_data.is_ortho = 1;
                    SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));

                    pm->RenderPassStandardBody(cb, &rp, bm, 0, &push_data);

                    auto cp = SDL_BeginGPUCopyPass(cb);
                    SDL_GPUTextureLocation src = {
                        .texture = rp.renderPassTexsData.depthTargetInfo.texture
                    };
                    SDL_GPUTextureLocation dst = {
                        .texture = flat_array->texture_binding.texture,
                        .layer = camera_index
                    };
                    SDL_CopyGPUTextureToTexture(cp, &src, &dst, flat_array->width, flat_array->height, 1, false);
                    SDL_EndGPUCopyPass(cp);
                }
                camera_index++;
            }
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

void DefaultRenderPassNamespace::_SetDefaultCommonResources(EngineContext* ctx, uint32_t width, uint32_t height)
{
    if (g_pass_system.common_inited) {
        SDL_Log("PassSystem common resources are already initialized.");
        return;
    }
    TextureManager* tm = ctx->GetTextureManager();

    auto depth_tci = TexturePresets::GetCreateInfo(TexturePreset::SingleDepth2048);
    depth_tci.width = width;
    depth_tci.height = height;

    g_pass_system.main_depth = tm->CreateSharedDepthTarget(depth_tci);
    g_pass_system.main_depth_format = depth_tci.format;
    tm->main_pass_depth = g_pass_system.main_depth;

    g_pass_system.common_inited = true;
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
    if (!g_pass_system.common_inited) {
        SDL_Log("SetDefaultMainRenderPass: _SetDefaultCommonResources must be called first.");
        return;
    }
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    RenderPassTexturesInfo main_rptd{};
    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.1f,0.1f,0.14f,1.0f }, SDL_GPU_TEXTUREFORMAT_INVALID);
    main_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, g_pass_system.main_depth_format);

    auto mainPass = pm->CreateRenderPass(
        MAIN_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(main_rptd),
        20
    );


    mainPass->global_texture_bindings = {
        shadow_depth_flat_array->texture_binding,           // слот 0 (t0/s0) — тень
        GetDefaultEnvBinding(ctx)                           // слот 1 (t1/s1) — env-кубмапа
    };
    mainPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);

    main_pass_inited = true;
}

void DefaultRenderPassNamespace::SetDebugColliderPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    if (!main_pass_inited || !g_pass_system.common_inited) {
        SDL_Log("SetDebugColliderPass: MAIN_PASS / common resources must be initialized first.");
        return;
    }

    // Рисуем ПОВЕРХ свопчейна: цвет грузим (LOAD), не чистим. Формат INVALID → формат
    // свопчейна подставит PipeManager (MakeDefaultColorTarget). Depth ЗАГРУЖАЕМ из
    // MAIN_PASS (LOAD) — дебаг-шейдер тестит его (ReadsDepthOnly), поэтому рамки
    // перекрываются геометрией сцены. depth_write выключен → рамки не портят буфер.
    // Формат depth берём из общего PassSystem — он обязан совпадать с форматом текстуры.
    RenderPassTexturesInfo debug_rptd{};
    debug_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, SDL_GPU_TEXTUREFORMAT_INVALID);
    debug_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_DONT_CARE, g_pass_system.main_depth_format);

    auto debugPass = pm->CreateRenderPass(
        DEBUG_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        // Цвет (свопчейн) ставится в Engine::RenderFunc каждый кадр; если не задан — пропуск.
        if (rp.renderPassTexsData.colorTargetInfos.empty() || !rp.renderPassTexsData.colorTargetInfos[0].texture) return;
        // Цвет рамок прокидываем как push_data_raw — его читает push_func дебаг-шейдера
        // (fragment slot 0). Другие программы к этому пассу не привязаны.
        DebugColliderPushData color{};
        pm->RenderPassStandardBody(cb, &rp, bm, 0, &color);
    },
        std::move(debug_rptd),
        25
    );

    debugPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);
}

void DefaultRenderPassNamespace::SetTransparentPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetRenderManager();
    BufferManager* bm = ctx->GetBufferManager();

    if (!main_pass_inited || !g_pass_system.common_inited) {
        SDL_Log("SetTransparentPass: MAIN_PASS / common resources must be initialized first.");
        return;
    }

    // Цвет грузим (LOAD) — рисуем поверх непрозрачной сцены в свопчейне. Depth ЗАГРУЖАЕМ
    // из MAIN_PASS и только тестим (depth_write выключен у программы) — прозрачная
    // геометрия корректно перекрывается непрозрачной, но не портит depth-буфер.
    // Блендинг включается на стороне программы через BehavesAsTransparentGeometry.
    // Формат depth — общий из PassSystem.
    RenderPassTexturesInfo transparent_rptd{};
    transparent_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, SDL_GPU_TEXTUREFORMAT_INVALID);
    transparent_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_DONT_CARE, g_pass_system.main_depth_format);

    auto transparentPass = pm->CreateRenderPass(
        TRANSPARENT_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        // Цвет (свопчейн) ставится в Engine::RenderFunc каждый кадр; если не задан — пропуск.
        if (rp.renderPassTexsData.colorTargetInfos.empty() || !rp.renderPassTexsData.colorTargetInfos[0].texture) return;
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(transparent_rptd),
        22   // между MAIN_PASS (20) и DEBUG_PASS (25)
    );

    transparentPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);
}

//void DefaultRenderPassNamespace::SetDefaultMainRenderPass(EngineContext* ctx,
//    SDL_GPUDevice* dev, SDL_Window* win)
//{
//    if (main_pass_inited) {
//        SDL_Log("Default main render pass is already initialized.");
//        return;
//    }
//    if (!shadow_pass_inited) {
//        SDL_Log("Default shadow render pass must be initialized before the default main render pass.");
//        return;
//    }
//    PassManager* pm = ctx->GetRenderManager();
//    BufferManager* bm = ctx->GetBufferManager();
//	TextureManager* tm = ctx->GetTextureManager();
//
//    const SDL_GPUTextureFormat sc_format = SDL_GetGPUSwapchainTextureFormat(dev, win);
//
//    int win_w = 0, win_h = 0;
//    SDL_GetWindowSizeInPixels(win, &win_w, &win_h);
//    split_half_w = (Uint32)win_w / 2;
//    split_full_h = (Uint32)win_h;
//
//    // --- offscreen-таргеты, один раз ---
//    SDL_GPUTextureCreateInfo color_ci{};
//    color_ci.type = SDL_GPU_TEXTURETYPE_2D;
//    color_ci.format = sc_format;                          // == swapchain, чтобы copy шёл без конвертации
//    color_ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;  // для copy-source отдельный флаг не нужен
//    color_ci.width = split_half_w;
//    color_ci.height = split_full_h;
//    color_ci.layer_count_or_depth = 1;
//    color_ci.num_levels = 1;
//    color_ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
//    main_color_half = tm->CreateGPU_Texture(color_ci);
//    debug_color_half = tm->CreateGPU_Texture(color_ci);
//
//    auto depth_ci = TexturePresets::GetCreateInfo(TexturePreset::SingleDepth2048);
//    depth_ci.width = split_half_w;
//    depth_ci.height = split_full_h;
//    split_depth_half = tm->CreateGPU_Texture(depth_ci);
//
//    // --- SCENE_PASS: бывший MAIN_PASS, рисует сцену в левую половину ---
//    RenderPassTexturesInfo scene_rptd{};
//    scene_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.1f,0.1f,0.14f,1.0f }, sc_format);
//    scene_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, depth_ci.format);
//
//    auto scenePass = pm->CreateRenderPass(
//        "SCENE_PASS",
//        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
//    {
//        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
//    },
//        std::move(scene_rptd),
//        20
//    );
//    scenePass->global_texture_bindings = {
//        shadow_depth_flat_array->texture_binding,
//        GetDefaultEnvBinding(ctx)                           // env-кубмапа на слоте 1 (как в MAIN_PASS)
//    };
//    scenePass->renderPassTexsData.SetColorTexture(main_color_half);
//    scenePass->renderPassTexsData.SetDepthTexture(split_depth_half);
//
//    // --- DEBUG_PASS: затычка с такой же color+depth-структурой, рисует в правую половину ---
//    RenderPassTexturesInfo debug_rptd{};
//    debug_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.14f,0.1f,0.1f,1.0f }, sc_format);
//    debug_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_DONT_CARE, depth_ci.format);
//
//    auto debugPass = pm->CreateRenderPass(
//        "DEBUG_PASS",
//        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
//    {
//        // ЗАТЫЧКА: пока та же сцена; сюда позже воткнёшь debug-пайплайн
//        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
//    },
//        std::move(debug_rptd),
//        25
//    );
//    debugPass->global_texture_bindings = { shadow_depth_flat_array->texture_binding };
//    debugPass->renderPassTexsData.SetColorTexture(debug_color_half);
//    debugPass->renderPassTexsData.SetDepthTexture(split_depth_half);
//
//    // --- MAIN_PASS: теперь copy-step в swapchain ---
//    RenderPassTexturesInfo main_rptd{};
//    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, SDL_GPU_TEXTUREFORMAT_INVALID);
//    // depth не нужен — это copy, а не render pass
//
//    auto mainPass = pm->CreateRenderPass(
//        MAIN_PASS,
//        [](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
//    {
//        // swapchain поставлен в RenderFunc через SetColorTexture(tex)
//        SDL_GPUTexture* swap = rp.renderPassTexsData.colorTargetInfo.texture; // (или [0].texture, если массив)
//        if (!swap || !main_color_half || !debug_color_half) return;
//
//        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
//
//        SDL_GPUTextureLocation src_left{};  src_left.texture = main_color_half;
//        SDL_GPUTextureLocation dst_left{};   dst_left.texture = swap; dst_left.x = 0;             dst_left.y = 0;
//        SDL_CopyGPUTextureToTexture(cp, &src_left, &dst_left, split_half_w, split_full_h, 1, false);
//
//        SDL_GPUTextureLocation src_right{}; src_right.texture = debug_color_half;
//        SDL_GPUTextureLocation dst_right{};  dst_right.texture = swap; dst_right.x = split_half_w; dst_right.y = 0;
//        SDL_CopyGPUTextureToTexture(cp, &src_right, &dst_right, split_half_w, split_full_h, 1, false);
//
//        SDL_EndGPUCopyPass(cp);
//    },
//        std::move(main_rptd),
//        30
//    );
//
//    main_pass_inited = true;
//}


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
        Uint32 camera_index = 0;      // общий для всех типов света
        Uint32 sphere_layer = 0;      // только для sphere

        auto flat_array = tm->GetTextureAtlas(SHADOW_MOMENTS_ARRAY);
        uint32_t commands_byte_offset = bb->AskNumCommands() * sizeof(SDL_GPUIndexedIndirectDrawCommand);
        om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SpotLightComponent& light, ShadowCasterComponent& sc) {
            //SDL_Log("Shadow pass: camera_index=%u, spotLayer=%u", camera_index, spotLayer);
            uint32_t byte_offset = (1 + camera_index) * commands_byte_offset;
            if (light.needsUpdate) {
                SDL_PushGPUVertexUniformData(cb, 0, &camera_index, sizeof(Uint32));
                rp.renderPassTexsData.SetColorTargetInfoLayer(camera_index, 0);
                pm->RenderPassStandardBody(cb, &rp, bm, 0, &camera_index);
            };
            camera_index++;
        }
        );

        om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(om->GetActiveScene(),
            [&](Positions& pos_el, SphereLightComponent& light, ShadowCasterComponent& sc) {
            for (int face = 0; face < 6; ++face) {
                uint32_t byte_offset = (1 + camera_index) * commands_byte_offset;
                if (light.needsUpdate) {
                    SDL_PushGPUVertexUniformData(cb, 0, &camera_index, sizeof(Uint32));

                    rp.renderPassTexsData.SetColorTargetInfoLayer(camera_index, 0);
                    pm->RenderPassStandardBody(cb, &rp, bm, 0, &camera_index);
                }
                camera_index++;
            }

            sphere_layer++;
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

