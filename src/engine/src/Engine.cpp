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
#include <bit>

namespace {

// Вложенных списков формат не знает: mat_lodL — список по части на элемент, а состояния — плоские
// списки, где к какой части относится состояние, говорит state_part.
auto MakeSaveRenderable(MaterialManager* mtm, ModelManager* mdm) {
return [mtm, mdm](Archetype& arch, size_t count, sheaf::Writer& w, std::vector<sheaf::Column>& out)
{
    const Renderable& r = arch.get_array<Renderable>()->data;
    const RenderableProxy def;
    auto column = [](std::string name, sheaf::Type type, uint8_t flags, uint32_t def_value) {
        sheaf::Column c;
        c.name = std::move(name); c.type = type; c.flags = flags; c.def = def_value;
        return c;
    };

    sheaf::Column vis = column("visible", sheaf::Type::Bool, 0, def.visible);
    sheaf::Column alp = column("alpha",   sheaf::Type::F32,  0, sheaf::Bits(def.alpha));
    sheaf::Column flg = column("flags",   sheaf::Type::U32,  0, def.flags);
    sheaf::Column mdl = column("model",   sheaf::Type::Str,  0, w.Intern(mdm->ModelNameOf(def.model)));
    for (size_t i = 0; i < count; ++i) {
        vis.values.push_back(r.visible[i] != 0);
        alp.values.push_back(sheaf::Bits(r.alpha[i]));
        flg.values.push_back(r.flags[i]);
        mdl.values.push_back(w.Intern(mdm->ModelNameOf(r.model[i])));
    }
    out.push_back(std::move(vis));
    out.push_back(std::move(alp));
    out.push_back(std::move(flg));
    out.push_back(std::move(mdl));

    for (uint32_t L = 0; L < MAX_LOD; ++L) {
        sheaf::Column lod = column("mat_lod" + std::to_string(L), sheaf::Type::Str, sheaf::List | sheaf::Nullable, 0);
        bool any = false;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t n = std::min(mdm->LevelCount(r.model[i]), MAX_LOD);
            lod.lengths.push_back(safe_u32(r.materials[i].size()));
            for (const MaterialSlot& part : r.materials[i]) {
                const MaterialId m = L < n ? part.per_lod[L] : MaterialId{};
                lod.values.push_back(m ? w.Intern(mtm->MaterialNameOf(m)) : 0);
                lod.present.push_back(static_cast<bool>(m));
                any = any || static_cast<bool>(m);
            }
        }
        if (any || L == 0) out.push_back(std::move(lod));   // по длинам mat_lod0 загрузка узнаёт число частей
    }

    sheaf::Column st_part  = column("state_part",  sheaf::Type::U32, sheaf::List, 0);
    sheaf::Column st_role  = column("state_role",  sheaf::Type::U32, sheaf::List, 0);
    sheaf::Column st_value = column("state_value", sheaf::Type::U32, sheaf::List, 0);
    bool any_state = false;
    for (size_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        for (size_t p = 0; p < r.materials[i].size(); ++p)
            for (const auto& [role, v] : r.materials[i][p].states) {
                st_part.values.push_back(safe_u32(p));
                st_role.values.push_back(safe_i_u32(static_cast<int>(role)));
                st_value.values.push_back(v);
                ++n;
            }
        st_part.lengths.push_back(n);
        st_role.lengths.push_back(n);
        st_value.lengths.push_back(n);
        any_state = any_state || n > 0;
    }
    if (!any_state) return;
    out.push_back(std::move(st_part));
    out.push_back(std::move(st_role));
    out.push_back(std::move(st_value));
};}

auto MakeLoadRenderable(MaterialManager* mtm, ModelManager* mdm) {
return [mtm, mdm](Archetype& arch, const sheaf::Component* comp, size_t count, std::span<const std::string> strings)
{
    arch.ensure_component<Renderable>();
    arch.get_array<Renderable>()->reserve(arch.entities.size());
    std::vector<RenderableProxy> rows(count);
    auto column = [comp](const std::string& name, sheaf::Type type, uint8_t flags) -> const sheaf::Column* {
        if (!comp) return nullptr;
        for (const sheaf::Column& c : comp->fields) {
            if (c.name != name) continue;
            if (c.type == type && c.flags == flags) return &c;
            SDL_Log("LoadScene: Renderable.%s in file has unexpected type - defaults kept", name.c_str());
            return nullptr;
        }
        return nullptr;
    };
    // Номер строки файла → id: одно имя повторяется у тысяч объектов, интернируется оно один раз.
    std::vector<ModelId>    model_of(strings.size());
    std::vector<MaterialId> material_of(strings.size());
    std::vector<uint8_t>    model_done(strings.size()), material_done(strings.size());
    auto model = [&](uint32_t s) {
        if (!model_done[s]) { model_of[s] = mdm->InternModel(strings[s]); model_done[s] = 1; }
        return model_of[s];
    };
    auto material = [&](uint32_t s) {
        if (!material_done[s]) { material_of[s] = mtm->InternMaterial(strings[s]); material_done[s] = 1; }
        return material_of[s];
    };

    if (const sheaf::Column* c = column("visible", sheaf::Type::Bool, 0))
        for (size_t i = 0; i < count; ++i) rows[i].visible = c->values[i] != 0;
    if (const sheaf::Column* c = column("alpha", sheaf::Type::F32, 0))
        for (size_t i = 0; i < count; ++i) rows[i].alpha = std::bit_cast<float>(c->values[i]);
    if (const sheaf::Column* c = column("flags", sheaf::Type::U32, 0))
        for (size_t i = 0; i < count; ++i) rows[i].flags = c->values[i];
    if (const sheaf::Column* c = column("model", sheaf::Type::Str, 0))
        for (size_t i = 0; i < count; ++i) rows[i].model = model(c->values[i]);

    for (uint32_t L = 0; L < MAX_LOD; ++L) {
        const sheaf::Column* c = column("mat_lod" + std::to_string(L), sheaf::Type::Str, sheaf::List | sheaf::Nullable);
        if (!c) continue;
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            std::vector<MaterialSlot>& parts = rows[i].materials;
            const uint32_t n = c->lengths[i];
            if (parts.size() < n) parts.resize(n);
            for (uint32_t p = 0; p < n; ++p, ++k)
                if (c->present[k]) parts[p].per_lod[L] = material(c->values[k]);
        }
    }

    const sheaf::Column* st_part  = column("state_part",  sheaf::Type::U32, sheaf::List);
    const sheaf::Column* st_role  = column("state_role",  sheaf::Type::U32, sheaf::List);
    const sheaf::Column* st_value = column("state_value", sheaf::Type::U32, sheaf::List);
    if (st_part && st_role && st_value) {
        size_t kp = 0, kr = 0, kv = 0;
        for (size_t i = 0; i < count; ++i) {
            // Колонки разъехались (файл собран не движком) — лишнее молча отбрасываем.
            const uint32_t n = std::min({ st_part->lengths[i], st_role->lengths[i], st_value->lengths[i] });
            std::vector<MaterialSlot>& parts = rows[i].materials;
            for (uint32_t q = 0; q < n; ++q) {
                const uint32_t p = st_part->values[kp + q];
                if (p >= parts.size()) continue;
                parts[p].states.emplace_back(static_cast<TextureSlotRole>(safe_u32t_i(st_role->values[kr + q])),
                                             st_value->values[kv + q]);
            }
            kp += st_part->lengths[i];
            kr += st_role->lengths[i];
            kv += st_value->lengths[i];
        }
    }

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
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.visible[i] = v != 0.0 ? 1 : 0; },
				nullptr)
				.Cmd(CommandId::HideEntity),
			FieldSpec::Num("alpha", F32,
				[](Archetype& a, size_t i) -> double { return a.get_array<Renderable>()->data.alpha[i]; },
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.alpha[i] = static_cast<float>(v); },
				nullptr, 0, 1, 0.01f),
			FieldSpec::Num("flags", U32,
				[](Archetype& a, size_t i) -> double { return a.get_array<Renderable>()->data.flags[i]; },
				[](Archetype& a, size_t i, double v) { a.get_array<Renderable>()->data.flags[i] = static_cast<uint32_t>(v); },
				nullptr),
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
	DefaultShaderProgramSet::SetFroxelFogPrograms(engine_context);
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
		SetTransparentPass(engine_context, light_data_module);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);
		// Якорь у тумана тот же, что у bloom (DEBUG_PASS): созданный позже встаёт раньше, то есть до bloom.
		SetDefaultFroxelFogPass(engine_context, light_data_module);
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
	pass_manager->FillRenderPasses();
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

	// Фенсы последних кадров отпускают UploadFunc/FenceFunc, а их потоки уже остановлены.
	SlotData* slots = slot_controller->GetSlotsData();
	for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
		for (StageFences* stage : { &slots[i].upload, &slots[i].render }) {
			for (uint8_t f = 0; f < stage->count; ++f)
				SDL_ReleaseGPUFence(dev, stage->items[f]);
			stage->Clear();
		}
	}

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
