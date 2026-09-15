#include "PCH.h"
#include "Engine.h"
// Engine.h теперь только forward-декларации — полные типы менеджеров тянет этот TU.
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
#include "ParamsSpec.h"
#include "PositionStructure.h"
#include "DefaultCommandSet.h"
#include "UI_ImGui.h"

//  Engine: конструирование/разрушение + инициализация дефолтов.
//  Кадровый конвейер — Engine_Frame.cpp; save/load сцены — Engine_Scene.cpp;
//  регистрация UI-команд — DefaultCommandSet.cpp.

void Engine::OnWindowResized(Sint32 window_w, Sint32 window_h)
{
	// Публикуем ТОЛЬКО размер окна: он свойство платформы и больше ничьё, а внутреннее разрешение —
	// производное от него и GraphicsConfig, и хранить его отдельно значило бы завести второй источник
	// истины. Пересоздание таргетов из этого следует, но делает его гейт RenderFunc: удалять текстуры
	// вправе только render-поток. Событие resized летит сотнями за drag, но атомик коалесит поток,
	// и дороже записи одного числа здесь ничего нет.
	size_state_.window_size.store(EngineSizeState::Pack(safe_i_u32(window_w), safe_i_u32(window_h)),
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

	// ТОЛЬКО SPIRV, и это НЕ настройка: движок компилирует шейдеры единственным путём
	// (LoadOrCompileSPIRV → SDL_ShaderCross_CompileSPIRVFromHLSL), а compute-пайплайны отдаёт в SDL
	// сырым SPIR-V (PipeManager::GetOrCreateComputePipeline). Перечислить тут DXIL/MSL — значит
	// разрешить SDL выбрать бэкенд, для которого у нас нет байткода: на SDL 3.4 авто-выбор на
	// Windows уходит в D3D12, и все compute-пайплайны падают на «not valid DXIL». Запрос ровно того
	// формата, который мы умеем, — и есть контракт; SDL сам подберёт подходящий бэкенд.
	dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, cfg.gpu_debug, nullptr);
	// Отказ здесь раньше не проверялся, и поломка проявлялась каскадом «Must claim window
	// before…» из последующих запросов свопчейна — то есть симптомом, а не причиной.
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
	// Не из конфига: глубина конвейера слотов — устройство движка (SlotController/BUFF_LVL),
	// расхождение с ней здесь рассинхронизирует кадры в полёте со слотами.
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
	// Платформа ПЕРВЫМ делом: менеджеры принимают dev в конструкторах, без девайса создавать
	// нечего. Отказ оставляет объект невалидным (init_ok=false) — dtor это учитывает.
	if (!InitPlatform(cfg)) return;
	// Настройки графики — до любого создания таргетов и до первого GetWidth: из них выводятся размеры.
	// Копия, а не ссылка на cfg: дальше их правят в рантайме, и переживать временный EngineConfig
	// они обязаны.
	graphics_config = new GraphicsConfig{ cfg.graphics };
	// cfg задаёт размер ОКНА, и это единственный размер, который движок хранит: внутреннее разрешение
	// из него и конфига выводится на месте (GetWidth/GetHeight, замыкания ресайза).
	size_state_.window_size.store(EngineSizeState::Pack(cfg.width, cfg.height), std::memory_order_relaxed);
	// Гейт стартует «уже применённым»: таргеты создаст _SetDefaultCommonResources под эти же входы.
	applied_inputs_ = TargetSizeInputs{ *graphics_config, cfg.width, cfg.height };
	transfer_manager = new TransferManager(dev);
	queue_manager = new QueueManager(dev);
	buffer_manager = new BufferManager(dev, transfer_manager);
	texture_manager = new TextureManager(dev, transfer_manager);
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
	font_manager = new FontManager();   // TTF_Init/Quit — в его ctor/dtor

	batch_builder = new BatchBuilder();

	pib_data_module = new PIB_DataModule();
	transform_data_module = new TransformDataModule();
	instance_data_module = new InstanceDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	bound_sphere_data_module = new BoundSphereDataModule();
	tex_state_data_module = new TextureStateDataModule();
	ui_data_module = new UI_DataModule();
	ui_yoga = new UI_Yoga();   // flex-раскладка UI (Yoga) → UI-энтити; Emit в PrepareFunc

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	engine_context->SetFontManager(font_manager);   // кроссменеджерский CreateFont (см. CLAUDE.md)
	engine_context->SetUIYoga(ui_yoga);   // игра берёт его отсюда для декларативной сборки UI
	engine_context->SetEngine(this);   // делегирование Save/LoadScene (оркестрация сцены-папки)
	engine_context->SetGraphicsConfig(graphics_config);   // замыкания ресайза выводят из него размеры
	// Пул движковой раскладки — ПЕРВЫМ из всего, что связано с геометрией: он заводит буферы стримов
	// и индексный, а заодно вешает их инструкции заливки. Всё дальнейшее (вершинники, объявляющие
	// usage, и модели) уже ссылается на него по имени.
	engine_context->CreateGeometryPool(POS_UV_NORM_POOL, sizeof(PosUVNormal), PosUVNormLayout());
	InitDefaultBufferUpdaters();
	InitPasses();
	DefaultCommandSet::SetAll(*input_manager);
	RegisterBuiltinComponentSpecs();          // спецификации компонентов:  save/load сцены + схема полей для UI
	RegisterBuiltinMaterialParamsSpecs();     // спецификации params материалов: то же самое для блоба факторов
	// Staging-сцена формы создания энтити (UI_Hierarchy): НИКОГДА не активна — дата-модули и
	// батчи её не видят, поэтому UI-поток монопольно правит её содержимое. Создаётся здесь,
	// до старта потоков: карту сцен после старта не мутируем (GetActiveScene её итерирует).
	object_manager->CreateScene("_staging")->is_active = false;
	pass_manager->FillRenderPasses();

	thread_controller->SetPrepareCallback([this](uint8_t slot){this->PrepareFunc(slot);});
	thread_controller->SetUploadCallback([this](uint8_t slot) {this->UploadFunc(slot); });
	thread_controller->SetComputeCallback([this](uint8_t slot) {this->ComputeFunc(slot); });
	thread_controller->SetRenderCallback(
		[this](uint8_t slot) {
			return this->RenderFunc(slot);   // !!! return
		}
	);
	thread_controller->SetFenceCallback([this](uint8_t slot) {this->FenceFunc(slot); });

	UI_ImGui::Init(win, dev);
	DefaultResourceSet::SetDefaultResources(engine_context);
	DefaultShaderProgramSet::SetDefaultShaders(engine_context);
	// Бейк GPU-ресурсов здесь НЕ делаем: игра объявляет свои ресурсы и шейдерные программы позже
	// (атласы в Game::Init, sp — в манифесте сцены), а именно объявления sp несут usage-флаги.
	// Точка бейка — конец первого Engine::LoadScene.
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

	// GPU-каллинг с компактацией: сферы по строкам + entity->cmd (ревизия батчей) +
	// ресайз out_pib (компактно пишет scatter-каллинг). Индирект — per-frame выше.
	SetDefaultBoundSphereUpdater(*engine_context, bound_sphere_data_module);
	SetDefaultEntityToCmdUpdater(*engine_context, pib_data_module);
	SetDefaultOutPibUpdater(*engine_context, light_data_module);

	// Переключаемые варианты текстур: префикс по строкам + плоские ячейки состояний, ОДИН модуль
	// на оба буфера. Пока ни одна sp их не объявила, обе инструкции — бесплатный no-op: буфер без
	// usage не бейкается, и _ExecuteUpdateInstructions гейтит инструкцию целиком, даже не считая
	// size_fn. Оживут сами, когда буферы попадут в списки sp.
	SetDefaultTexStateUpdaters(*engine_context, tex_state_data_module);

	// UI-текст: bits/wordbase/index/text (UI_DataModule) + GlyphUVL (FontManager, шрифт "default").
	// Буферы бейкаются, когда программа "UI" объявит их usage (InitDefaultShaders, ниже по Init).
	SetUITextUpdaters(*engine_context, ui_data_module, font_manager, "default");
}

void Engine::InitPasses()
{
	using namespace DefaultRenderPassNamespace;

	{
		// Размер НАЗНАЧЕНИЯ (окно): внутреннее разрешение таргетов _SetDefaultCommonResources выведет
		// из него и конфига сам — теми же функциями, что и замыкания ресайза.
		_SetDefaultCommonResources(engine_context, safe_f_u32(GetWindowWidth()), safe_f_u32(GetWindowHeight()));
		SetDefaultCullingPass(engine_context);     // GPU-каллинг: out_pib до SHADOW_PASS (индекс 5)
		SetDefaultShadowPCFRenderPass(engine_context, light_data_module);
		SetDefaultMainRenderPass(engine_context, light_data_module);
		SetDefaultAOPass(engine_context);           // SSAO по глубине main'а, применяется до тумана
		//SetDefaultFogPass(engine_context);          // атмосфера по глубине main'а: ПОСЛЕ AO, до прозрачных
		// SetDefaultSplatPass(engine_context);  ВЫКЛЮЧЕН: сплат — это терминальный уровень LOD, и
		// строить его раньше самой LOD-цепочки оказалось преждевременно. Код прохода, шейдеры и
		// перевёрнутый тест каллинга оставлены на месте; чтобы включить обратно, нужны эта строка,
		// программа "Splat" ниже в InitDefaultShaders и её sp в списке материала.
		SetTransparentPass(engine_context, light_data_module);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);       // bloom от эмиссии (compute) + composite/tonemap в scene_hdr
		SetUIPass(engine_context);                 // UI-оверлей (NDC-квады) в scene_hdr после bloom, до present
		SetPresentPass(engine_context);            // финал: HDR-сцену в свопчейн (blit)
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
	// Потоки поднимаются ЗДЕСЬ, а не в конструкторе: между конструированием движка и стартом
	// конвейера игра успевает создать свои ресурсы и сцену (MainInit). Sim-поток пошёл бы по ним
	// раньше, чем они появились.
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
				// window-пара из события; render-пара (0,0) пока не используется — внутреннее
				// разрешение зафиксировано в движке, картинка тянется на окно present-блитом.
				OnWindowResized(event.window.data1, event.window.data2);

			// Весь игровой ввод — в очередь IM, дренит sim-поток.
			input_manager->HandleEvent(event);
		}
		SDL_Delay(16);
	}

	// ДО возврата, а не в dtor движка: игровой колбэк, который крутит sim-поток, замкнут на объект
	// игры, живущий у вызывающего Run() и разрушаемый сразу после него. Вернуться с живыми потоками
	// = дать sim позвать метод уже разрушенной игры.
	thread_controller->Shutdown();
	return 0;
}

Engine::~Engine()
{
	if (!init_ok) {
		// Конструктор оборвался на платформе: менеджеров нет, ImGui не поднимался — рушим
		// только то, что успело появиться (оба Destroy терпят nullptr).
		SDL_DestroyGPUDevice(dev);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return;
	}
	// Первым делом и здесь: dtor вправе сработать без Run() (ранний выход игры), а ниже удаляются
	// менеджеры, по которым ходят потоки конвейера. Повторный вызов после Run() — no-op.
	thread_controller->Shutdown();

	UI_ImGui::Shutdown();

	// Первым: он не владеет ничем, а держит сырые ссылки на всё, что удаляется ниже.
	delete engine_context;

	delete buffer_manager;
	delete texture_manager;
	delete transfer_manager;   // после менеджеров: они возвращают арендованные TB в пул
	delete queue_manager;      // ничем не владеет (очереди принадлежат устройству) — порядок свободный
	delete shader_manager;
	delete pipe_manager;
	delete model_manager;
	delete pass_manager;
	delete object_manager;
	delete camera_manager;
	// ThreadController — СТРОГО раньше SlotController: он держит на него сырой указатель и в своём
	// dtor зовёт NotifyShutdown() (остановка потоков). При обратном порядке это лочило мьютекс уже
	// освобождённой памяти — в Release прокатывало (байты ещё «те самые»), в Debug куча забита 0xDD
	// и остановка вставала намертво. Раньше не всплывало: dtor Engine вообще не вызывался, main
	// выходил через `return 0`.
	delete thread_controller;
	delete slot_controller;
	delete material_manager;
	delete input_manager;
	delete texture_loader;
	delete font_manager;   // dtor: TTF_CloseFont всех шрифтов + TTF_Quit
	delete batch_builder;
	delete pib_data_module;
	delete transform_data_module;
	delete instance_data_module;
	delete light_data_module;
	delete indirect_data_module;
	delete bound_sphere_data_module;
	delete tex_state_data_module;
	delete ui_data_module;
	delete ui_yoga;   // YGNodeFreeRecursive дерева + YGConfigFree (в его dtor)
	delete graphics_config;

	// Платформу подняли мы (InitPlatform) — мы же её и рушим. Строго после менеджеров и ImGui:
	// они держат ресурсы устройства, а release окна должен опережать уничтожение девайса.
	SDL_ReleaseWindowFromGPUDevice(dev, win);
	SDL_DestroyGPUDevice(dev);
	SDL_DestroyWindow(win);
	SDL_Quit();

	dev = nullptr;
	win = nullptr;
}
