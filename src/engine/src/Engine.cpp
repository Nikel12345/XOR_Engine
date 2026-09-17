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
#include "ModelManager.h"
#include "InputManager.h"
#include "FontManager.h"
#include "TextureLoader.h"
#include "BatchBuilder.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "InstanceDataModule.h"
#include "TextureStateDataModule.h"
#include "LightDataModule.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"
#include "UI_DataModule.h"
#include "UI_Yoga.h"
#include "EngineContext.h"
#include "DefaultUpdateSet.h"
#include "DefaultRenderPassSet.h"
#include "DefaultShaderSet.h"
#include "DefaultResourceSet.h"
#include "ComponentSerializer.h"
#include "BaseComponents.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ParamsSpec.h"
#include "PositionStructure.h"
#include "DefaultCommandSet.h"
#include "UI_ImGui.h"

namespace {

auto MakeSaveMaterial(MaterialManager* mtm) {
return [mtm](Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp, ScenePool* pool)
{
    auto& arr = *arch.get_array<MaterialComponent>();
    // Имя списка литералом, а не через FieldPoolName: у Material нет FieldSpec, из которого его
    // взять.
    ScenePool::List* list = pool ? &(*pool)["materials"] : nullptr;
    yyjson_mut_val* col = yyjson_mut_obj_add_arr(doc, comp, "names");
    bool any_state = false;
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, col);
        for (const MaterialRef& m : arr[i].materials) {
            if (list) yyjson_mut_arr_add_uint(doc, row, list->Intern(mtm->MaterialNameOf(m.material)));
            else      yyjson_mut_arr_add_strcpy(doc, row, mtm->MaterialNameOf(m.material).c_str());
            any_state = any_state || !m.states.empty();
        }
    }
    // Состояния вариантов — ОТДЕЛЬНАЯ колонка, параллельная "names" по позиции материала, и её
    // нет вовсе, пока никто ничего не переключал. Внутри плоский список пар (роль, вариант), роль
    // числом: её строковые имена ECS не знает.
    if (!any_state) return;
    yyjson_mut_val* scol = yyjson_mut_obj_add_arr(doc, comp, "states");
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, scol);
        for (const MaterialRef& m : arr[i].materials) {
            yyjson_mut_val* pairs = yyjson_mut_arr_add_arr(doc, row);
            for (const auto& [role, v] : m.states) {
                yyjson_mut_arr_add_int(doc, pairs, static_cast<int>(role));
                yyjson_mut_arr_add_uint(doc, pairs, v);
            }
        }
    }
}
;}

auto MakeLoadMaterial(MaterialManager* mtm) {
return [mtm](Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool)
{
    arch.ensure_component<MaterialComponent>();
    std::vector<MaterialComponent> rows(count);
    ScenePool::List* list = pool ? pool->Find("materials") : nullptr;
    yyjson_val* col = comp ? yyjson_obj_get(comp, "names") : nullptr;
    if (col) {
        size_t idx, max; yyjson_val* row;
        yyjson_arr_foreach(col, idx, max, row) {
            if (idx >= count) break;
            size_t j, jm; yyjson_val* s;
            yyjson_arr_foreach(row, j, jm, s) {
                const char* str = pool ? pool->Cell(list, s) : yyjson_get_str(s);
                if (str) rows[idx].materials.push_back(MaterialRef{ mtm->InternMaterial(str), {} });
            }
        }
    }
    // Колонки может не быть — тогда states пусты и каждый слот показывает дефолт.
    yyjson_val* scol = comp ? yyjson_obj_get(comp, "states") : nullptr;
    if (scol) {
        size_t idx, max; yyjson_val* row;
        yyjson_arr_foreach(scol, idx, max, row) {
            if (idx >= count) break;
            size_t j, jm; yyjson_val* pairs;
            yyjson_arr_foreach(row, j, jm, pairs) {
                if (j >= rows[idx].materials.size()) break;   // колонки разъехались — лишнее молча отбрасываем
                std::vector<std::pair<TextureSlotRole, uint32_t>>& st = rows[idx].materials[j].states;
                size_t k, km; yyjson_val* v;
                // Плоские пары: нечётный хвост (файл правили руками) отбрасываем целиком.
                const size_t n = yyjson_arr_size(pairs) & ~size_t(1);
                std::vector<int64_t> flat; flat.reserve(n);
                yyjson_arr_foreach(pairs, k, km, v) { if (flat.size() >= n) break; flat.push_back(yyjson_get_sint(v)); }
                for (size_t p = 0; p + 1 < flat.size(); p += 2)
                    st.emplace_back(static_cast<TextureSlotRole>(flat[p]),
                                    static_cast<uint32_t>(flat[p + 1] < 0 ? 0 : flat[p + 1]));
            }
        }
    }
    auto* a = arch.get_array<MaterialComponent>();
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);
};}

} // namespace


static void RegisterResourceComponentSpecs(MaterialManager* mtm, ModelManager* mdm)
{
	ComponentSpecRegistry::Get().Register({ .name = "Material", .sig_type = typeid(MaterialComponent),
		.add_default = AddDefaultAoS<MaterialComponent>,
		.custom_save = MakeSaveMaterial(mtm), .custom_load = MakeLoadMaterial(mtm) });

	ComponentSpecRegistry::Get().Register({ .name = "Model", .sig_type = typeid(ModelComponent),
		.add_default = AddDefaultAoS<ModelComponent>,
		// Смена модели меняет состав батчей И число сабмешей, то есть длину списка материалов:
		// одной записью строки с UI-потока не обойтись, отсюда .Cmd.
		.fields = { FieldSpec::Str("name",
			[mdm](Archetype& a, size_t i) -> const std::string& {
				return mdm->ModelNameOf((*a.get_array<ModelComponent>())[i].model);
			},
			[mdm](Archetype& a, size_t i, std::string v) {
				(*a.get_array<ModelComponent>())[i].model = mdm->InternModel(v);
			},
			FieldKind::AssetModel).Cmd(CommandId::SetEntityModel) } });
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

	auto make_window = [&cfg] {
		return SDL_CreateWindow(cfg.title, safe_u32t_i(cfg.width), safe_u32t_i(cfg.height), cfg.window_flags);
	};
	win = make_window();
	if (!win) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}

	dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, cfg.gpu_debug, nullptr);
	if (!dev) {
		SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
		return false;
	}
	SDL_Log("GPU backend: %s", SDL_GetGPUDeviceDriver(dev));
	SDL_ClaimWindowForGPUDevice(dev, win);

	// БАГ SDL 3.4.14: ПЕРВОЕ созданное в процессе окно Vulkan-девайс не заклеймливает —
	// ClaimWindowForGPUDevice возвращает true, но окно не регистрируется, и дальше весь свопчейн
	// отвечает «Must claim window before…». Второе окно клеймится штатно. Воспроизведено голым
	// SDL, без движка: sandbox/src/ClaimWindowProbe.cpp (там же отсеяны ложные версии — способ
	// выбора бэкенда, debug_mode, SDL_WINDOW_VULKAN, порядок создания девайсов: ни при чём).
	// Поэтому проверяем ФАКТ (формат свопчейна), а не возврат claim, и один раз пересоздаём окно.
	// Условная ветка: когда баг починят, она просто перестанет срабатывать.
	if (SDL_GetGPUSwapchainTextureFormat(dev, win) == SDL_GPU_TEXTUREFORMAT_INVALID) {
		SDL_Log("Claim didn't take (SDL 3.4 first-window bug) - recreating window");
		SDL_ReleaseWindowFromGPUDevice(dev, win);
		SDL_DestroyWindow(win);
		win = make_window();
		if (!win || !SDL_ClaimWindowForGPUDevice(dev, win)) {
			SDL_Log("Window re-claim failed: %s", SDL_GetError());
			return false;
		}
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
	graphics_config = new GraphicsConfig{ cfg.graphics };
	size_state.window_size.store(EngineSizeState::Pack(cfg.width, cfg.height), std::memory_order_relaxed);
	applied_inputs = TargetSizeInputs{ *graphics_config, cfg.width, cfg.height };
	transfer_manager = new TransferManager(dev);
	queue_manager = new QueueManager(dev);
	buffer_manager = new BufferManager(dev, transfer_manager);
	texture_manager = new TextureManager(dev, transfer_manager);
	DefaultResourceSet::CreateDefaultBuffers(buffer_manager);
	DefaultResourceSet::CreateDefaultTextureResources(texture_manager);
	shader_manager = new ShaderManager(dev);
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
	transform_data_module = new TransformDataModule();
	instance_data_module = new InstanceDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	bound_sphere_data_module = new BoundSphereDataModule();
	tex_state_data_module = new TextureStateDataModule();
	ui_data_module = new UI_DataModule();
	ui_yoga = new UI_Yoga();

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	engine_context->SetFontManager(font_manager);
	engine_context->SetUIYoga(ui_yoga);
	engine_context->SetEngine(this);
	engine_context->SetGraphicsConfig(graphics_config);
	engine_context->CreateGeometryPool(POS_UV_NORM_POOL, sizeof(PosUVNormal), PosUVNormLayout());
	InitDefaultBufferUpdaters();
	InitPasses();
	DefaultCommandSet::SetAll(*input_manager);
	RegisterBuiltinComponentSpecs();
	RegisterResourceComponentSpecs(material_manager, model_manager);
	RegisterBuiltinMaterialParamsSpecs();
	object_manager->CreateScene("staging")->is_active = false;
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

	SetDefaultBoundSphereUpdater(*engine_context, bound_sphere_data_module);
	SetDefaultEntityToCmdUpdater(*engine_context, pib_data_module);
	SetDefaultOutPibUpdater(*engine_context, light_data_module);

	SetDefaultTexStateUpdaters(*engine_context, tex_state_data_module);

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
		// SetDefaultSplatPass(engine_context);  ВЫКЛЮЧЕН: сплат — это терминальный уровень LOD, и
		// строить его раньше самой LOD-цепочки оказалось преждевременно. Код прохода, шейдеры и
		// перевёрнутый тест каллинга оставлены на месте; чтобы включить обратно, нужны эта строка,
		// программа "Splat" ниже в InitDefaultShaders и её sp в списке материала.
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
	delete transform_data_module;
	delete instance_data_module;
	delete light_data_module;
	delete indirect_data_module;
	delete bound_sphere_data_module;
	delete tex_state_data_module;
	delete ui_data_module;
	delete ui_yoga;
	delete graphics_config;

	SDL_ReleaseWindowFromGPUDevice(dev, win);
	SDL_DestroyGPUDevice(dev);
	SDL_DestroyWindow(win);
	SDL_Quit();

	dev = nullptr;
	win = nullptr;
}
