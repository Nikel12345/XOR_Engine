#include "DefaultRenderPassSet.h"
#include "TextureManager.h"
#include "RenderManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "ObjectManager.h"
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
        TextureAtlas*        scene_hdr = nullptr;       // HDR-цвет сцены (location 0 MAIN_PASS), общий для MAIN/TRANSPARENT/DEBUG
        TextureAtlas*        scene_emission = nullptr;  // HDR-эмиссия (location 1 MAIN_PASS, MRT) — источник bloom
        // Bloom-пирамида: ОТДЕЛЬНАЯ текстура на уровень "bloom_L<i>" (уровень 0 = ½ окна, дальше /2).
        // Не мипы одной текстуры: dst-уровень биндится RW, src — сэмплером, отдельные текстуры
        // исключают одновременный RW+sampled бинд одной (см. TexturesPresets::BloomLevel).
        TextureAtlas*        bloom_levels[BLOOM_LEVELS] = {};
        uint32_t             width = 0;                 // размер HDR-таргетов (= окно на момент init); нужен present-blit'у
        uint32_t             height = 0;
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
	PassManager* pm = ctx->GetPassManager();
	BufferManager* bm = ctx->GetBufferManager();
	ObjectManager* om = ctx->GetObjectManager();
	BatchBuilder* bb = ctx->GetBatchBuilder();   // num_instances для адресации блоков out_pib

    auto shadow_sampler = tm->GetSampler(DefaultSamplersNames::DEFAULT_SHADOW_SAMPLER);
    auto shadow_tci = TexturePresets::GetCreateInfo(TexturePreset::Depth_FlatArray1024_16Layers);
    uint32_t max_layers = shadow_tci.layer_count_or_depth;

    shadow_depth_flat_array = tm->CreateTextureAtlas(SHADOW_DEPTH_FLAT_ARRAY, shadow_tci, shadow_sampler);
    TextureAtlas* shadow_temp = tm->CreateTextureAtlas("shadow_depth_single_temp", TexturePresets::GetCreateInfo(TexturePreset::TempDepth1024), shadow_sampler);

    RenderPassTexturesInfo shadow_rptd{};
    shadow_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, shadow_temp->format);

    auto shadowPass = pm->CreateRenderPass(
        SHADOW_PASS,
        [pm, bm, om, tm, bb, max_layers](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
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
                push_data.num_instances = bb->AskNumInstances();
                SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));
                pm->RenderPassStandardBody(cb, &rp, bm, (1u + camera_index) * bb->AskNumCommands() * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand)), &push_data);

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
                    push_data.num_instances = bb->AskNumInstances();
                    SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));

                    pm->RenderPassStandardBody(cb, &rp, bm, (1u + camera_index) * bb->AskNumCommands() * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand)), &push_data);

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
                    push_data.num_instances = bb->AskNumInstances();
                    SDL_PushGPUVertexUniformData(cb, 0, &push_data, sizeof(ShadowPushData));

                    pm->RenderPassStandardBody(cb, &rp, bm, (1u + camera_index) * bb->AskNumCommands() * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand)), &push_data);

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

    // HDR-таргеты набора: сцена рендерится в линейный HDR (эмиссия/блики уходят за 1.0), на экран
    // выводится финальным present-проходом (blit). scene_emission — второй MRT-выход main-прохода,
    // источник bloom. scene_hdr сэмплится bloom-prefilter'ом (13-тап) → нужен LINEAR + clamp, как у
    // эмиссии и уровней bloom (compute-фильтры). На present-blit фильтр сэмплера не влияет.
    auto env_sampler = tm->GetSampler(DefaultSamplersNames::ENV_SAMPLER);
    g_pass_system.scene_hdr      = tm->CreateTextureAtlas("scene_hdr",      TexturePresets::SceneHDR(width, height),    env_sampler);
    g_pass_system.scene_emission = tm->CreateTextureAtlas("scene_emission", TexturePresets::EmissionHDR(width, height), env_sampler);

    // Bloom-пирамида: BLOOM_LEVELS отдельных текстур "bloom_L<i>". Уровень 0 = ½ окна, дальше /2.
    for (uint32_t i = 0; i < BLOOM_LEVELS; ++i) {
        uint32_t lw = width  >> (1 + i); if (lw == 0) lw = 1;
        uint32_t lh = height >> (1 + i); if (lh == 0) lh = 1;
        g_pass_system.bloom_levels[i] = tm->CreateTextureAtlas("bloom_L" + std::to_string(i),
            TexturePresets::BloomLevel(lw, lh), env_sampler);
    }

    g_pass_system.width  = width;
    g_pass_system.height = height;

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
    PassManager* pm = ctx->GetPassManager();
    BufferManager* bm = ctx->GetBufferManager();

    // Два color-таргета (MRT): location 0 — HDR-цвет сцены, location 1 — эмиссия. Форматы берём
    // из созданных атласов (PipeManager строит пайплайн по этому массиву → согласовано с фрагментом,
    // у которого теперь два SV_Target). Раньше color был INVALID (= свопчейн); теперь сцена в HDR.
    RenderPassTexturesInfo main_rptd{};
    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.1f,0.1f,0.14f,1.0f }, g_pass_system.scene_hdr->format);
    main_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE, { 0.0f,0.0f,0.0f,0.0f }, g_pass_system.scene_emission->format);
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
    // Цвет привязан в setup (не per-frame): сцена рендерится в свои HDR-таргеты постоянно,
    // свопчейн теперь получает только present-проход.
    mainPass->renderPassTexsData.SetColorTexture(g_pass_system.scene_hdr->texture_binding.texture, 0);
    mainPass->renderPassTexsData.SetColorTexture(g_pass_system.scene_emission->texture_binding.texture, 1);
    mainPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);

    main_pass_inited = true;
}

void DefaultRenderPassNamespace::SetDebugColliderPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetPassManager();
    BufferManager* bm = ctx->GetBufferManager();

    if (!main_pass_inited || !g_pass_system.common_inited) {
        SDL_Log("SetDebugColliderPass: MAIN_PASS / common resources must be initialized first.");
        return;
    }

    // Рисуем ПОВЕРХ HDR-сцены: цвет грузим (LOAD) из scene_hdr, не чистим. Формат = scene_hdr
    // (раньше INVALID/свопчейн — теперь сцена в HDR, present выводит её отдельно). Depth ЗАГРУЖАЕМ
    // из MAIN_PASS (LOAD) — дебаг-шейдер тестит его (ReadsDepthOnly), поэтому рамки перекрываются
    // геометрией сцены. depth_write выключен → рамки не портят буфер.
    RenderPassTexturesInfo debug_rptd{};
    debug_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, g_pass_system.scene_hdr->format);
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

    // Цвет — общий HDR-таргет сцены (привязан в setup, не per-frame).
    debugPass->renderPassTexsData.SetColorTexture(g_pass_system.scene_hdr->texture_binding.texture, 0);
    debugPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);
}

void DefaultRenderPassNamespace::SetTransparentPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetPassManager();
    BufferManager* bm = ctx->GetBufferManager();

    if (!main_pass_inited || !g_pass_system.common_inited) {
        SDL_Log("SetTransparentPass: MAIN_PASS / common resources must be initialized first.");
        return;
    }

    RenderPassTexturesInfo transparent_rptd{};
    transparent_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, g_pass_system.scene_hdr->format);
    transparent_rptd.CreateDepthTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_DONT_CARE, g_pass_system.main_depth_format);

    auto transparentPass = pm->CreateRenderPass(
        TRANSPARENT_PASS,
        [pm, bm](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        if (rp.renderPassTexsData.colorTargetInfos.empty() || !rp.renderPassTexsData.colorTargetInfos[0].texture) return;
        pm->RenderPassStandardBody(cb, &rp, bm, 0, nullptr);
    },
        std::move(transparent_rptd),
        22   // между MAIN_PASS (20) и DEBUG_PASS (25)
    );

    transparentPass->renderPassTexsData.SetColorTexture(g_pass_system.scene_hdr->texture_binding.texture, 0);
    transparentPass->renderPassTexsData.SetDepthTexture(g_pass_system.main_depth);
}

void DefaultRenderPassNamespace::SetPresentPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetPassManager();

    if (!g_pass_system.common_inited) {
        SDL_Log("SetPresentPass: common resources must be initialized first.");
        return;
    }

    // Финал кадра: HDR-сцена → свопчейн. Именно blit (а не copy-pass) — форматы разные
    // (R16G16B16A16_FLOAT → формат свопчейна 8-бит), blit конвертирует на лету. Значения > 1.0
    // пока просто клампятся; полноценный тонмаппинг появится в bloom-composite. Один color-слот
    // нужен лишь чтобы Engine::RenderFunc мог положить сюда свопчейн (texture index 0) —
    // BeginGPURenderPass мы не зовём, всё делает SDL_BlitGPUTexture.
    RenderPassTexturesInfo present_rptd{};
    present_rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_LOAD, SDL_GPU_STOREOP_STORE, { 0,0,0,1 }, SDL_GPU_TEXTUREFORMAT_INVALID);

    pm->CreateRenderPass(
        PRESENT_PASS,
        [](SDL_GPUCommandBuffer* cb, PassManager* pm, RenderPassStep& rp)
    {
        SDL_GPUTexture* swap = rp.renderPassTexsData.colorTargetInfos.empty()
            ? nullptr : rp.renderPassTexsData.colorTargetInfos[0].texture;
        if (!swap || !g_pass_system.scene_hdr) return;

        SDL_GPUBlitInfo blit{};
        blit.source.texture      = g_pass_system.scene_hdr->texture_binding.texture;
        blit.source.w            = g_pass_system.width;
        blit.source.h            = g_pass_system.height;
        blit.destination.texture = swap;
        blit.destination.w       = g_pass_system.width;
        blit.destination.h       = g_pass_system.height;
        blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter              = SDL_GPU_FILTER_NEAREST;
        blit.cycle               = false;
        SDL_BlitGPUTexture(cb, &blit);
    },
        std::move(present_rptd),
        30   // последним, после MAIN(20)/TRANSPARENT(22)/DEBUG(25)
    );
}

void DefaultRenderPassNamespace::ResizeSceneHDRTargets(EngineContext* ctx, uint32_t width, uint32_t height)
{
    if (!g_pass_system.common_inited || !g_pass_system.scene_hdr) return;
    if (g_pass_system.width == width && g_pass_system.height == height) return;
    if (width == 0 || height == 0) return;

    TextureManager* tm = ctx->GetTextureManager();
    PassManager* pm = ctx->GetPassManager();

    auto recreate = [&](TextureAtlas* atlas, SDL_GPUTextureCreateInfo tci) {
        SDL_GPUTexture* old_tex = atlas->texture_binding.texture;
        atlas->texture_binding.texture = tm->CreateGPU_Texture(tci);
        atlas->width  = tci.width;        // из tci: bloom-уровни меньше окна
        atlas->height = tci.height;
        tm->QueueDeleteTexture(old_tex);
    };
    recreate(g_pass_system.scene_hdr, TexturePresets::SceneHDR(width, height));
    recreate(g_pass_system.scene_emission, TexturePresets::EmissionHDR(width, height));

    for (uint32_t i = 0; i < BLOOM_LEVELS; ++i) {
        if (!g_pass_system.bloom_levels[i]) continue;
        uint32_t lw = width  >> (1 + i); if (lw == 0) lw = 1;
        uint32_t lh = height >> (1 + i); if (lh == 0) lh = 1;
        recreate(g_pass_system.bloom_levels[i], TexturePresets::BloomLevel(lw, lh));
    }
    g_pass_system.width = width;
    g_pass_system.height = height;

    // Перебиндить новые текстуры во все проходы, что в них пишут (texture* поменялся).
    SDL_GPUTexture* hdr = g_pass_system.scene_hdr->texture_binding.texture;
    SDL_GPUTexture* emi = g_pass_system.scene_emission->texture_binding.texture;
    if (RenderPassStep* mp = pm->GetRenderPassStep(MAIN_PASS)) {
        mp->renderPassTexsData.SetColorTexture(hdr, 0);
        mp->renderPassTexsData.SetColorTexture(emi, 1);
    }
    if (RenderPassStep* tp = pm->GetRenderPassStep(TRANSPARENT_PASS))
        tp->renderPassTexsData.SetColorTexture(hdr, 0);
    if (RenderPassStep* dp = pm->GetRenderPassStep(DEBUG_PASS))
        dp->renderPassTexsData.SetColorTexture(hdr, 0);
}

void DefaultRenderPassNamespace::SetDefaultBloomPass(EngineContext* ctx)
{
    if (!g_pass_system.common_inited || !g_pass_system.scene_emission) {
        SDL_Log("SetDefaultBloomPass: common resources must be initialized first.");
        return;
    }
    PassManager* pm = ctx->GetPassManager();
    BufferManager* bm = ctx->GetBufferManager();

    pm->CreateComputePass(
        BLOOM_PASS,
        [bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
    {
        BloomParams      push{};
        DummyDispatchData dd{};
        pm->ComputePassStandardBody(cb, &cp, bm, &push, &dd, pass_frame);
    },
        26
    );
}

void DefaultRenderPassNamespace::SetDefaultShadowVSMRenderPass(EngineContext* ctx)
{
    if (shadow_pass_inited) {
        SDL_Log("Default shadow render pass is already initialized.");
        return;
    }
    PassManager* pm = ctx->GetPassManager();
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
    PassManager* pm = ctx->GetPassManager();
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

void DefaultRenderPassNamespace::SetDefaultCullingPass(EngineContext* ctx)
{
    PassManager* pm = ctx->GetPassManager();
    BufferManager* bm = ctx->GetBufferManager();

    // GPU-каллинг (scatter) — PREPASS: Engine::RenderFunc гоняет его в ОТДЕЛЬНОМ cb + fence
    // ПЕРЕД рендером. Так out_pib и per-camera indirect гарантированно дописаны до чтения их
    // draw'ами (SDL_GPU не барьерит compute-write -> indirect-read в одном cb; на 1М scatter
    // медленный от atomic-контеншена, и draw читал недозаполненный индирект → мерцание).
    pm->CreateComputePrepass(
        CULLING_PASS,
        [bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
    {
        CullingPibUniform push{};
        DummyDispatchData dd{};
        pm->ComputePassStandardBody(cb, &cp, bm, &push, &dd, pass_frame);
    },
        5
    );
}

