#include "PCH.h"
#include "Engine.h"
#include "QueueManager.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "ShaderManager.h"
#include "PipeManager.h"
#include "ModelManager.h"
#include "PassManager.h"
#include "ObjectManager.h"
#include "CameraManager.h"
#include "SlotController.h"
#include "ThreadController.h"
#include "MaterialManager.h"
#include "InputManager.h"
#include "FontManager.h"
#include "TextureLoader.h"
#include "BatchBuilder.h"
#include "PIB_DataModule.h"
#include "BoundSphereDataModule.h"
#include "CullingDataModule.h"
#include "TransformDataModule.h"
#include "InstanceDataModule.h"
#include "TextureStateDataModule.h"
#include "LightDataModule.h"
#include "IndirectDataModule.h"
#include "UI_DataModule.h"
#include "UI_Yoga.h"
#include "EngineContext.h"
#include "DefaultUpdateSet.h"
#include "DefaultRenderPassSet.h"
#include "DefaultShaderSet.h"
#include "DefaultResourceSet.h"
#include "ComponentSerializer.h"
#include "BaseComponents.h"
#include "ParamsSpec.h"
#include "PositionStructure.h"
#include "DefaultCommandSet.h"
#include "UI_ImGui.h"

namespace {

// states: роль числом — её строковые имена ECS не знает.
auto MakeSaveRenderable(MaterialManager* mtm, ModelManager* mdm) {
return [mtm, mdm](Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp, ScenePool* pool)
{
    const Renderable& r = arch.get_array<Renderable>()->data;
    ScenePool::List* mat_list = pool ? &(*pool)["materials"] : nullptr;
    ScenePool::List* mdl_list = pool ? &(*pool)["models"] : nullptr;
    auto add_name = [doc](yyjson_mut_val* arr, ScenePool::List* list, const std::string& name) {
        if (list) yyjson_mut_arr_add_uint(doc, arr, list->Intern(name));
        else      yyjson_mut_arr_add_strcpy(doc, arr, name.c_str());
    };

    yyjson_mut_val* vis = yyjson_mut_obj_add_arr(doc, comp, "visible");
    yyjson_mut_val* alp = yyjson_mut_obj_add_arr(doc, comp, "alpha");
    yyjson_mut_val* flg = yyjson_mut_obj_add_arr(doc, comp, "flags");
    yyjson_mut_val* mdl = yyjson_mut_obj_add_arr(doc, comp, "model");
    yyjson_mut_val* mat = yyjson_mut_obj_add_arr(doc, comp, "materials");
    bool any_state = false;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t n = std::min(mdm->LevelCount(r.model[i]), MAX_LOD);
        yyjson_mut_arr_add_bool(doc, vis, r.visible[i] != 0);
        yyjson_mut_arr_add_real(doc, alp, r.alpha[i]);
        yyjson_mut_arr_add_uint(doc, flg, r.flags[i]);
        add_name(mdl, mdl_list, mdm->ModelNameOf(r.model[i]));

        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, mat);
        for (const MaterialSlot& part : r.materials[i]) {
            yyjson_mut_val* lv = yyjson_mut_arr_add_arr(doc, row);
            for (uint32_t L = 0; L < n; ++L) {
                if (part.per_lod[L]) add_name(lv, mat_list, mtm->MaterialNameOf(part.per_lod[L]));
                else                 yyjson_mut_arr_add_null(doc, lv);
            }
            any_state = any_state || !part.states.empty();
        }
    }
    if (!any_state) return;
    yyjson_mut_val* scol = yyjson_mut_obj_add_arr(doc, comp, "states");
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, scol);
        for (const MaterialSlot& part : r.materials[i]) {
            yyjson_mut_val* pairs = yyjson_mut_arr_add_arr(doc, row);
            for (const auto& [role, v] : part.states) {
                yyjson_mut_arr_add_int(doc, pairs, static_cast<int>(role));
                yyjson_mut_arr_add_uint(doc, pairs, v);
            }
        }
    }
}
;}

auto MakeLoadRenderable(MaterialManager* mtm, ModelManager* mdm) {
return [mtm, mdm](Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool)
{
    arch.ensure_component<Renderable>();
    std::vector<RenderableProxy> rows(count);
    ScenePool::List* mat_list = pool ? pool->Find("materials") : nullptr;
    ScenePool::List* mdl_list = pool ? pool->Find("models") : nullptr;
    auto name_of = [pool](ScenePool::List* list, yyjson_val* v) -> const char* {
        return pool ? pool->Cell(list, v) : yyjson_get_str(v);
    };
    auto rows_of = [&](const char* key, auto&& fn) {
        yyjson_val* col = comp ? yyjson_obj_get(comp, key) : nullptr;
        if (!col) return;
        size_t idx, max; yyjson_val* v;
        yyjson_arr_foreach(col, idx, max, v) { if (idx >= count) break; fn(rows[idx], v); }
    };

    rows_of("visible", [](RenderableProxy& p, yyjson_val* v) { p.visible = yyjson_get_bool(v); });
    rows_of("alpha",   [](RenderableProxy& p, yyjson_val* v) { p.alpha = static_cast<float>(yyjson_get_num(v)); });
    rows_of("flags",   [](RenderableProxy& p, yyjson_val* v) { p.flags = safe_u32(yyjson_get_uint(v)); });
    rows_of("model", [&](RenderableProxy& p, yyjson_val* v) {
        if (const char* s = name_of(mdl_list, v)) p.model = mdm->InternModel(s);
    });
    rows_of("materials", [&](RenderableProxy& p, yyjson_val* row) {
        size_t k, km; yyjson_val* lv;
        yyjson_arr_foreach(row, k, km, lv) {
            MaterialSlot& part = p.materials.emplace_back();
            size_t L, lm; yyjson_val* v;
            yyjson_arr_foreach(lv, L, lm, v) {
                if (L >= MAX_LOD) break;
                if (yyjson_is_null(v)) continue;
                if (const char* s = name_of(mat_list, v)) part.per_lod[L] = mtm->InternMaterial(s);
            }
        }
    });
    rows_of("states", [](RenderableProxy& p, yyjson_val* row) {
        size_t j, jm; yyjson_val* pairs;
        yyjson_arr_foreach(row, j, jm, pairs) {
            if (j >= p.materials.size()) break;   // колонки разъехались — лишнее молча отбрасываем
            auto& st = p.materials[j].states;
            // Плоские пары: нечётный хвост (файл правили руками) отбрасываем целиком.
            const size_t n = yyjson_arr_size(pairs) & ~size_t(1);
            std::vector<int64_t> flat; flat.reserve(n);
            size_t k, km; yyjson_val* v;
            yyjson_arr_foreach(pairs, k, km, v) { if (flat.size() >= n) break; flat.push_back(yyjson_get_sint(v)); }
            for (size_t q = 0; q + 1 < flat.size(); q += 2)
                st.emplace_back(static_cast<TextureSlotRole>(flat[q]),
                                static_cast<uint32_t>(flat[q + 1] < 0 ? 0 : flat[q + 1]));
        }
    });

    auto* a = arch.get_array<Renderable>();
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);
};}

} // namespace


static void RegisterResourceComponentSpecs(MaterialManager* mtm, ModelManager* mdm)
{
	using enum FieldKind;
	ComponentSpecRegistry::Get().Register({ .name = "Renderable", .sig_type = typeid(Renderable),
		.add_default = AddDefaultSoA<Renderable, RenderableProxy>,
		.fields = {
			// Прямая запись флага не поставит дельту в батчи, поэтому правка уходит командой.
			FieldSpec::Num("visible", Bool,
				[](Archetype& a, size_t i) -> double { return a.get_array<Renderable>()->data.visible[i]; },
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.visible[i] = v != 0.0 ? 1 : 0; })
				.Cmd(CommandId::HideEntity),
			FieldSpec::Num("alpha", F32,
				[](Archetype& a, size_t i) -> double { return a.get_array<Renderable>()->data.alpha[i]; },
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.alpha[i] = static_cast<float>(v); },
				0, 1, 0.01f),
			FieldSpec::Num("flags", U32,
				[](Archetype& a, size_t i) -> double { return a.get_array<Renderable>()->data.flags[i]; },
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.flags[i] = static_cast<uint32_t>(v); }),
		},
		.custom_save = MakeSaveRenderable(mtm, mdm), .custom_load = MakeLoadRenderable(mtm, mdm) });
}

void Engine::OnWindowResized(Sint32 window_w, Sint32 window_h)
{
	size_state.window_size.store(EngineSizeState::Pack(safe_i_u32(window_w), safe_i_u32(window_h)),
	                              std::memory_order_release);
}

bool Engine::InitPlatform(const EngineConfig& cfg)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return false;
	}

	win = SDL_CreateWindow(cfg.title, safe_u32t_i(cfg.width), safe_u32t_i(cfg.height), cfg.window_flags);
	if (!win) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}

	SDL_GPUVulkanOptions vk_options{};
	vk_options.vulkan_api_version = (cfg.vulkan_major << 22) | (cfg.vulkan_minor << 12);

	const SDL_PropertiesID dev_props = SDL_CreateProperties();
	SDL_SetBooleanProperty(dev_props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(dev_props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, cfg.gpu_debug);
	SDL_SetPointerProperty(dev_props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vk_options);
	dev = SDL_CreateGPUDeviceWithProperties(dev_props);
	SDL_DestroyProperties(dev_props);
	if (!dev) {
		SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
		char msg[128];
		SDL_snprintf(msg, sizeof(msg),
			"No suitable GPU found: Vulkan %u.%u is required.\nUpdating the graphics driver may help.",
			cfg.vulkan_major, cfg.vulkan_minor);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, cfg.title, msg, win);
		return false;
	}
	SDL_Log("GPU backend: %s", SDL_GetGPUDeviceDriver(dev));
	if (!SDL_ClaimWindowForGPUDevice(dev, win)) {
		SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
		return false;
	}
	SDL_SetGPUAllowedFramesInFlight(dev, BUFFERING_LEVEL);

	SDL_GPUPresentMode desired_mode = cfg.present_mode;
	SDL_GPUSwapchainComposition desired_comp = cfg.composition;
	if (!SDL_WindowSupportsGPUPresentMode(dev, win, desired_mode)) {
		SDL_Log("Present mode %d not supported - falling back to VSYNC", (int)desired_mode);
		desired_mode = SDL_GPU_PRESENTMODE_VSYNC;
	}
	if (!SDL_WindowSupportsGPUSwapchainComposition(dev, win, desired_comp)) {
		SDL_Log("Composition %d not supported - fallback to SDR", (int)desired_comp);
		desired_comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	}
	if (!SDL_SetGPUSwapchainParameters(dev, win, desired_comp, desired_mode))
		SDL_Log("Failed to set swapchain parameters: %s", SDL_GetError());
	else
		SDL_Log("Swapchain set: comp=%d, mode=%d", desired_comp, desired_mode);

	return true;
}

Engine::Engine(const EngineConfig& cfg)
{
	if (!InitPlatform(cfg)) return;
	size_state.window_size.store(EngineSizeState::Pack(cfg.width, cfg.height), std::memory_order_relaxed);
	transfer_manager = new TransferManager(dev);
	queue_manager = new QueueManager(dev);
	buffer_manager = new BufferManager(dev, transfer_manager);
	texture_manager = new TextureManager(dev, transfer_manager);
	DefaultResourceSet::CreateDefaultBuffers(buffer_manager);
	DefaultResourceSet::CreateDefaultTextureResources(texture_manager);
	shader_manager = new ShaderManager(dev, "vulkan" + std::to_string(cfg.vulkan_major) + "." + std::to_string(cfg.vulkan_minor));
	pipe_manager = new PipeManager(dev, win);
	model_manager = new ModelManager();
	pass_manager = new PassManager();
	object_manager = new ObjectManager();
	camera_manager = new CameraManager();
	slot_controller = new SlotController();
	thread_controller = new ThreadController(slot_controller);
	material_manager = new MaterialManager();
	input_manager = new InputManager();
	texture_loader = new TextureLoader();
	font_manager = new FontManager();

	batch_builder = new BatchBuilder();

	pib_data_module = new PIB_DataModule();
	bound_sphere_data_module = new BoundSphereDataModule();
	culling_data_module = new CullingDataModule();
	transform_data_module = new TransformDataModule();
	instance_data_module = new InstanceDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	tex_state_data_module = new TextureStateDataModule();
	ui_data_module = new UI_DataModule();
	ui_yoga = new UI_Yoga();

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	engine_context->SetFontManager(font_manager);
	engine_context->SetUIYoga(ui_yoga);
	engine_context->SetEngine(this);
	engine_context->CreateGeometryPool(POS_UV_NORM_POOL, sizeof(PosUVNormal), PosUVNormLayout());
	InitDefaultBufferUpdaters();
	InitPasses();
	DefaultCommandSet::SetAll(*input_manager);
	RegisterBuiltinComponentSpecs();
	RegisterResourceComponentSpecs(material_manager, model_manager);
	RegisterBuiltinMaterialParamsSpecs();
	object_manager->CreateScene("staging");
	pass_manager->FillRenderPasses();

	thread_controller->SetPrepareCallback([this](uint8_t slot){this->PrepareFunc(slot);});
	thread_controller->SetUploadCallback([this](uint8_t slot) {this->UploadFunc(slot); });
	thread_controller->SetComputeCallback([this](uint8_t slot) {this->ComputeFunc(slot); });
	thread_controller->SetRenderCallback(
		[this](uint8_t slot) {
			return this->RenderFunc(slot);
		}
	);
	thread_controller->SetFenceCallback([this](uint8_t slot) {this->FenceFunc(slot); });

	UI_ImGui::Init(win, dev);
	DefaultResourceSet::SetDefaultResources(engine_context);
	DefaultShaderProgramSet::SetDefaultShaders(engine_context);
	DefaultShaderProgramSet::SetCullingPrograms(engine_context, culling_data_module);
	DefaultShaderProgramSet::SetBloomPrograms(engine_context);
	DefaultShaderProgramSet::SetAOPrograms(engine_context);
	DefaultShaderProgramSet::SetFogProgram(engine_context);
	init_ok = true;
}

void Engine::InitDefaultBufferUpdaters()
{
	using namespace DefaultUpdateSet;

	SetDefaultCameraUpdater(*engine_context);
	SetDefaultPositionUpdater(*engine_context, transform_data_module);
	SetDefaultInstanceDataUpdater(*engine_context, instance_data_module);
	SetDefaultLightUpdater(*engine_context, light_data_module);
	SetDefaultPositionIndexUpdater(*engine_context, pib_data_module);
	SetDefaultLightCamerasUpdater(*engine_context, light_data_module);
	SetDefaultIndirectUpdater(*engine_context, indirect_data_module, light_data_module);
	SetDefaultCullingUpdaters(*engine_context, culling_data_module, bound_sphere_data_module);

	SetDefaultTexStateChannel(*engine_context, tex_state_data_module);
	SetDefaultTexStateUpdater(*engine_context, tex_state_data_module);

	SetUITextUpdaters(*engine_context, ui_data_module, font_manager, "default");
}

void Engine::InitPasses()
{
	using namespace DefaultRenderPassNamespace;

	{
		_SetDefaultCommonResources(engine_context, safe_f_u32(GetWindowWidth()), safe_f_u32(GetWindowHeight()));
		SetDefaultCullingPass(engine_context);
		SetDefaultShadowPCFRenderPass(engine_context, light_data_module);
		SetDefaultMainRenderPass(engine_context, light_data_module);
		SetDefaultAOPass(engine_context);
		//SetDefaultFogPass(engine_context);          // атмосфера по глубине main'а: ПОСЛЕ AO, до прозрачных
		SetTransparentPass(engine_context, light_data_module);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);
		SetUIPass(engine_context);
		SetPresentPass(engine_context);
	}
}

void Engine::SetGameIterate(std::function<void()> cb)
{
	thread_controller->SetGameIterationCallback(std::move(cb));
}

int Engine::Run()
{
	if (!init_ok) {
		SDL_Log("Engine::Run on an invalid engine (platform init failed)");
		return 1;
	}
	thread_controller->StartThreads();

	running.store(true, std::memory_order_relaxed);
	while (running.load(std::memory_order_relaxed)) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			UI_ImGui::ProcessEvent(event);

			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || event.type == SDL_EVENT_QUIT) {
				running.store(false, std::memory_order_relaxed);
				break;
			}
			if (event.type == SDL_EVENT_WINDOW_RESIZED)
				OnWindowResized(event.window.data1, event.window.data2);

			input_manager->HandleEvent(event);
		}
		SDL_Delay(16);
	}

	thread_controller->Shutdown();
	return 0;
}

Engine::~Engine()
{
	if (!init_ok) {
		SDL_DestroyGPUDevice(dev);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return;
	}
	thread_controller->Shutdown();

	UI_ImGui::Shutdown();

	delete engine_context;

	delete buffer_manager;
	delete texture_manager;
	delete transfer_manager;
	delete queue_manager;
	delete shader_manager;
	delete pipe_manager;
	delete model_manager;
	delete pass_manager;
	delete object_manager;
	delete camera_manager;
	delete thread_controller;
	delete slot_controller;
	delete material_manager;
	delete input_manager;
	delete texture_loader;
	delete font_manager;
	delete batch_builder;
	delete pib_data_module;
	delete bound_sphere_data_module;
	delete culling_data_module;
	delete transform_data_module;
	delete instance_data_module;
	delete light_data_module;
	delete indirect_data_module;
	delete tex_state_data_module;
	delete ui_data_module;
	delete ui_yoga;

	SDL_ReleaseWindowFromGPUDevice(dev, win);
	SDL_DestroyGPUDevice(dev);
	SDL_DestroyWindow(win);
	SDL_Quit();

	dev = nullptr;
	win = nullptr;
}
