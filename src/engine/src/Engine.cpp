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
#include "RenderManager.h"
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
#include "TexturesPresets.h"
#include "ComponentSerializer.h"
#include "ParamsSpec.h"
#include "PositionStructure.h"
#include "DefaultCommandSet.h"
#include "UI_ImGui.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include <filesystem>

using namespace ShaderBase;   // POSITION/UV/... в раскладке fallback-vs

//  Engine: конструирование/разрушение + инициализация дефолтов.
//  Кадровый конвейер — Engine_Frame.cpp; save/load сцены — Engine_Scene.cpp;
//  регистрация UI-команд — DefaultCommandSet.cpp.

void Engine::OnWindowResized(Sint32 window_w, Sint32 window_h, Sint32 render_w, Sint32 render_h)
{
	// Обновляем ТОЛЬКО window-пару. render-пара (width/height) ФИКСИРОВАНА — ресурсы, UI и камера
	// работают во внутреннем разрешении, а окно другого размера получает растянутую картинку финальным
	// present-блитом. Поэтому здесь больше НЕ трогаем render-размеры, НЕ ресайзим таргеты и НЕ дёргаем
	// UI (UI не перекладывается на ресайз окна — тянется блитом). Событие resized летит сотнями за drag,
	// но теперь это дёшево: только запись двух чисел.
	// Публикуем размер ОКНА для render-потока (свопчейн + present-блит). Таргеты этим НЕ трогаем:
	// смена окна — только презентация, картинка растягивается блитом render→окно. Атомик коалесит
	// поток resize-событий за drag.
	size_state_.window_size.store(EngineSizeState::Pack(static_cast<uint32_t>(window_w), static_cast<uint32_t>(window_h)),
	                              std::memory_order_release);
	// Будущая симметрия: публикация нового ВНУТРЕННЕГО разрешения (render_size) — пока вызывается тут.
	size_state_.render_size.store(EngineSizeState::Pack(static_cast<uint32_t>(window_w), static_cast<uint32_t>(window_h)),
		std::memory_order_release);

	(void)render_w;  (void)render_h;
}

void Engine::SetRenderResolution(uint32_t w, uint32_t h)
{
	// ЗАГОТОВКА (пока никто не зовёт): игровой UI (sim-поток) публикует новое ВНУТРЕННЕЕ разрешение —
	// render-поток подхватит по изменению render_size и пересоздаст таргеты (ExecuteResizeInstructions).
	// Удалять текстуры здесь (в sim) нельзя — поэтому только публикация.
	if (w == 0 || h == 0) return;
	size_state_.render_size.store(EngineSizeState::Pack(w, h), std::memory_order_release);
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
	const float width = safe_sint32_f(safe_u32t_i(cfg.width));
	const float height = safe_sint32_f(safe_u32t_i(cfg.height));
	// Стартовый размер (из конфига) кладём ТОЛЬКО в size_state_ — отдельных width/height в движке нет,
	// геттеры GetWidth/GetHeight читают render_size оттуда (единый источник истины размеров).
	// Экранные таргеты создаются в _SetDefaultCommonResources под этот стартовый размер, поэтому и
	// render_size, и applied стартуют «уже применёнными» — первый НАСТОЯЩИЙ ресайз (render или window) их сдвинет.
	const uint64_t start_size = EngineSizeState::Pack(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	size_state_.render_size.store(start_size, std::memory_order_relaxed);
	size_state_.window_size.store(start_size, std::memory_order_relaxed);
	size_state_.applied_render = start_size;   // гейт стартует «уже применённым» (таргеты созданы под этот размер)
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
	// Пул движковой раскладки — ПЕРВЫМ из всего, что связано с геометрией: он заводит буферы стримов
	// и индексный, а заодно вешает их инструкции заливки. Всё дальнейшее (вершинники, объявляющие
	// usage, и модели) уже ссылается на него по имени.
	engine_context->CreateGeometryPool(POS_UV_NORM_POOL, sizeof(PosUVNormal), PosUVNormLayout());
	InitDefaultBufferUpdaters();
	InitPasses();
	InitUICommands();
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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // docking-ветка ImGui: окна можно стыковать (см. будущий DockSpace)

	ImGui_ImplSDL3_InitForSDLGPU(win);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = dev;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&init_info);

	// Шрифт ImGui: дефолтный ProggyClean покрывает только латиницу, поэтому кириллические подписи
	// редактора (инспектор/иерархия) без этого рисуются знаками «?». Догружаем кириллицу МЕРЖ-режимом
	// из системного шрифта: латиница остаётся дефолтной (при пересечении глифов побеждает первый
	// добавленный шрифт), из Segoe UI берутся только кириллические глифы. ImGui 1.92 растеризует по
	// требованию (backend выставляет RendererHasTextures) — ручная сборка атласа не нужна. Путь
	// системный → только Windows и только если файл реально есть (иначе просто оставляем дефолт).
	io.Fonts->AddFontDefault();
#ifdef _WIN32
	{
		const char* sys_font = "C:/Windows/Fonts/segoeui.ttf";
		if (std::filesystem::exists(sys_font)) {
			ImFontConfig cfg;
			cfg.MergeMode = true;   // доклеить в дефолтный шрифт, а не заменить его
			io.Fonts->AddFontFromFileTTF(sys_font, 0.0f, &cfg, io.Fonts->GetGlyphRangesCyrillic());
		} else {
			SDL_Log("ImGui font: '%s' not found - Cyrillic editor labels will render as '?'", sys_font);
		}
	}
#endif

	engine_context->CreateTextureAtlas("_FallbackAtlas", TexturePresets::AlbedoAtlas(64, 1, 1), "_SimpleSampler");
	engine_context->CreateTextureFromFile("_NoTextureDummy", "_FallbackAtlas", "../engine/textures/dummy.png",
		ChannelConvention::AsIs, /*dont_save=*/true);   // движковый дефолт — в файл сцены не идёт

	// ПО ИМЕНИ (как SetFallbackShader): удаление _NoTextureDummy не оставляет висячего указателя —
	// промах на сборке батча даёт пропуск отрисовки (пустой рендер), а не разыменование мёртвого хэндла.
	batch_builder->SetDummyTexture("_NoTextureDummy", texture_manager);
	InitDefaultResources();
	InitDefaultShaders();
	// Бейк GPU-ресурсов здесь НЕ делаем: игра объявляет свои ресурсы и шейдерные программы позже
	// (атласы в Game::Init, sp — в манифесте сцены), а именно объявления sp несут usage-флаги.
	// Точка бейка — конец первого Engine::LoadScene.
	init_ok = true;
}

void Engine::InitDefaultResources()
{
	// Все material-атласы — BGRA8 UNORM (движок без sRGB-форматов), поэтому дефолты кладём в один
	// движковый _FallbackAtlas: цвет читается как есть, нормаль тоже (без sRGB-декода). Пиксели —
	// BGRA (все каналы равны → порядок неважен). Материалы ссылаются по имени, атлас безразличен.
	// dont_save: движковые дефолты пересоздаются кодом всегда — в файл сцены не идут (байтовые и
	// так скипались бы по пустому source_path, но флаг — явный маркер, не побочный эффект).
	TextureHandle* def_tex[] = {
		texture_manager->CreateTexture("default_albedo",   "_FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0xFF })),   // белый
		texture_manager->CreateTexture("default_normal",   "_FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0x80 })),   // 128,128,128,128 (высота в альфе)
		texture_manager->CreateTexture("default_orm",      "_FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF })),
		texture_manager->CreateTexture("default_emissive", "_FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF })),
	};
	for (TextureHandle* h : def_tex) if (h) h->dont_save = true;

	// Примитивы-дефолты движка (quad/sphere): генерируются кодом, поэтому dont_save (в models.json
	// не идут). Раньше жили в игре — вынесены сюда, чтобы любая игра/сцена могла ссылаться на них по
	// имени без своего кода генерации. Процедурные пути пусты → и без флага не сериализовались бы.
	// КАНОН развёртки: начало текстуры top-left (как грузит SDL_GPU и как рисует ImGui) → V идёт
	// ВНИЗ (v=0 у геометрического ВЕРХА). Верхние вершины (y=1) получают v=0 → верх картинки сверху.
	// Развёртка при этом левосторонняя относительно нормали — компенсируется глобально одним
	// cross(T,N) в main_pass.vert (не флаг). НЕ возвращай v-up: это перевернёт ориентированные текстуры.
	engine_context->CreateModel<PosUVNormal>("quad", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
		v = {
			{ 0,0,0,  0,1,  0,0,1,  1,0,0 },
			{ 1,0,0,  1,1,  0,0,1,  1,0,0 },
			{ 1,1,0,  1,0,  0,0,1,  1,0,0 },
			{ 0,1,0,  0,0,  0,0,1,  1,0,0 },
		};
		i = { 0, 1, 2, 0, 2, 3 };
	}, AnchorShift::Keep, /*dont_save=*/true);

	engine_context->CreateModel<PosUVNormal>("sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
		const uint32_t stacks = 32;   // деления по широте
		const uint32_t slices = 48;   // деления по долготе
		const float R = 1.0f;
		const float PI = 3.14159265358979323846f;

		for (uint32_t i = 0; i <= stacks; ++i) {
			float phi = PI * (float)i / (float)stacks;              // 0..π (полюс→полюс)
			float cp = std::cos(phi), sp = std::sin(phi);
			for (uint32_t j = 0; j <= slices; ++j) {
				float theta = 2.0f * PI * (float)j / (float)slices; // 0..2π
				float ct = std::cos(theta), st = std::sin(theta);

				float nx = sp * ct, ny = cp, nz = sp * st;          // нормаль = точка на единичной сфере
				PosUVNormal vert{};
				vert.x = R * nx; vert.y = R * ny; vert.z = R * nz;
				// U зеркалим (1-u): без этого надпись читалась ЗЕРКАЛЬНО (только изнутри сферы). V уже
				// v-down (v=0 у полюса φ=0 = верх картинки) — канон, не трогаем. Тангенс — вдоль НОВОГО
				// +U (∂pos/∂(−θ)) → знак θ-производной инвертируется, чтобы TBN совпал с cross(T,N).
				vert.u = 1.0f - (float)j / (float)slices;
				vert.v = (float)i / (float)stacks;
				vert.nx = nx; vert.ny = ny; vert.nz = nz;
				vert.tx = st; vert.ty = 0.0f; vert.tz = -ct;
				v.push_back(vert);
			}
		}

		const uint32_t row = slices + 1;
		for (uint32_t i = 0; i < stacks; ++i) {
			for (uint32_t j = 0; j < slices; ++j) {
				uint32_t a = i * row + j;
				uint32_t b = a + row;
				idx.push_back(a);     idx.push_back(a + 1); idx.push_back(b);
				idx.push_back(a + 1); idx.push_back(b + 1); idx.push_back(b);
			}
		}
	}, AnchorShift::Keep, /*dont_save=*/true);

	// Единичный куб (центр 0, полу-размер 1), v-down канон — как quad/sphere. Движковый примитив,
	// чтобы любая игра ссылалась по имени "cube" без своего кода генерации (был копией в mygame).
	// 6 граней, CCW наружу; тангенс = направление U; хранимый v = 1-параметр (позиция по исходному uv).
	engine_context->CreateModel<PosUVNormal>("cube", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
		struct FaceDef { float c[3], U[3], V[3], N[3]; };
		static const FaceDef faces[6] = {
			{{ 1,-1, 1}, { 0, 0,-2}, { 0, 2, 0}, { 1, 0, 0}},  // +X
			{{-1,-1,-1}, { 0, 0, 2}, { 0, 2, 0}, {-1, 0, 0}},  // -X
			{{-1, 1, 1}, { 2, 0, 0}, { 0, 0,-2}, { 0, 1, 0}},  // +Y
			{{-1,-1,-1}, { 2, 0, 0}, { 0, 0, 2}, { 0,-1, 0}},  // -Y
			{{-1,-1, 1}, { 2, 0, 0}, { 0, 2, 0}, { 0, 0, 1}},  // +Z
			{{ 1,-1,-1}, {-2, 0, 0}, { 0, 2, 0}, { 0, 0,-1}},  // -Z
		};
		const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
		for (int f = 0; f < 6; ++f) {
			const FaceDef& fd = faces[f];
			float tx = fd.U[0], ty = fd.U[1], tz = fd.U[2];
			const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
			if (tl > 0.0f) { tx /= tl; ty /= tl; tz /= tl; }
			const uint32_t vbase = static_cast<uint32_t>(v.size());
			for (int q = 0; q < 4; ++q) {
				PosUVNormal vert{};
				vert.x = fd.c[0] + uv[q][0]*fd.U[0] + uv[q][1]*fd.V[0];
				vert.y = fd.c[1] + uv[q][0]*fd.U[1] + uv[q][1]*fd.V[1];
				vert.z = fd.c[2] + uv[q][0]*fd.U[2] + uv[q][1]*fd.V[2];
				vert.u = uv[q][0]; vert.v = 1.0f - uv[q][1];   // v-down канон
				vert.nx = fd.N[0]; vert.ny = fd.N[1]; vert.nz = fd.N[2];
				vert.tx = tx;      vert.ty = ty;      vert.tz = tz;
				v.push_back(vert);
			}
			idx.push_back(vbase + 0); idx.push_back(vbase + 1); idx.push_back(vbase + 2);
			idx.push_back(vbase + 0); idx.push_back(vbase + 2); idx.push_back(vbase + 3);
		}
	}, AnchorShift::Keep, /*dont_save=*/true);

	// Фон сцены (скайбокс/фрактал) движковым дефолтом больше НЕ является: модель/шейдеры/материал/
	// текстура — ресурсы сцены (манифесты папки сцены), сам фон — сущность в её scene.json.
	// Классический скайбокс — src/game/saved_scene/scene1, фрактал — src/mygame/saved_scene/scene_fractal.
}

// Движковый набор шейдеров: вершинники/фрагментники/compute + render-программы, которыми рисуются
// штатные проходы. Раньше он ехал в манифесте сцены (game/saved_scene/scene1/shaders.json) — то есть каждая
// сцена возила КОПИЮ движковой инфраструктуры, а сцена без неё оставалась без базового рендера.
// Теперь это дефолтные ресурсы, как quad/sphere/cube и default_albedo: создаются кодом на старте,
// у всех dont_save (в shaders.json не пишутся и оттуда не грузятся).
//
// Цена резидентности замерена зондом sandbox/ShaderVramProbe.cpp: 0 байт VRAM и на шейдер, и на
// пайплайн; ~160 KB RAM драйвера на весь набор — против 5.3 MB у ОДНОЙ текстуры 1024² с мипами.
// Поэтому «создаём всегда, даже если сцена этим не рисует» здесь ничего не стоит.
//
// Сцена объявляет в своём манифесте только СВОИ шейдеры — те, которых движок не предусматривает
// (фрактальные фоны mygame: fractal_fs/anchor_surface_fs и их sp живут в манифесте своей сцены).
//
// Требует готовыми: пул геометрии (вершинники объявляют по нему usage буферов), буферы
// (InitDefaultBufferUpdaters) и проходы (InitPasses) — sp ссылается на проход по имени.
void Engine::InitDefaultShaders()
{
	using namespace DefaultBuffersNames;
	using namespace ShaderBase;
	namespace RP = DefaultRenderPassNamespace;

	// ── Fallback: материал с УДАЛЁННОЙ sp рисуется им (аналог untextured — цвет из params, без
	//    текстур). Держим ОТДЕЛЬНОЙ тройкой, а не ссылкой на main_pass_vs/untextured_surface_fs
	//    ниже: смысл fallback-а в том, чтобы пережить удаление любого шейдера из редактора.
	//    Одинаковый с ними байткод дедуплицируется по хэшу SPIR-V — второго GPU-шейдера не будет. ──
	engine_context->CreateVertexShader("_fallback_vs",
		"../engine/shaders_code/main_pass/main_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION, UV, NORMAL, TANGENT }, /*dont_save=*/true);
	engine_context->CreateFragmentShader("_fallback_fs",
		"../engine/shaders_code/main_pass/untextured/surface.hlsl", /*dont_save=*/true);
	{
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->DoesNotCull();
		engine_context->CreateShaderProgram("_Fallback", spd, RP::MAIN_PASS,
			"_fallback_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"_fallback_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ }, /*dont_save=*/true);   // текстур нет
		batch_builder->SetFallbackShader("_Fallback");   // ПО ИМЕНИ: удаление fallback → промах → пустой рендер
	}

	// ── Вершинники. Пул один (PosUVNorm), различаются НАБОРОМ семантик: теневому и скайбоксу
	//    хватает позиции, лишние стримы они не биндят (и не объявляют им VERTEX-usage). ──
	engine_context->CreateVertexShader("main_pass_vs", "../engine/shaders_code/main_pass/main_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION, UV, NORMAL, TANGENT }, /*dont_save=*/true);
	engine_context->CreateVertexShader("shadow_vs", "../engine/shaders_code/shadow_pass/shadow_pass.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, /*dont_save=*/true);
	engine_context->CreateVertexShader("skybox_vs", "../engine/shaders_code/skybox/skybox.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, /*dont_save=*/true);
	engine_context->CreateVertexShader("debug_collider_vs", "../engine/shaders_code/debug/debug_collider.vert.hlsl",
		POS_UV_NORM_POOL, { POSITION }, /*dont_save=*/true);

	// ── Фрагментники ──
	// Потолки раскладки вариантов уезжают в HLSL ДЕФАЙНАМИ, а не дублируются литералом: разъезд
	// C++ и байткода тихо перемешал бы секции состояний. Дефайны входят в ключ кэша .spv, поэтому
	// смена константы сама инвалидирует кэш. Набор отдаётся КАЖДОМУ fs, который включает пролог с
	// таблицей UVL, — забыть один значит собрать его на дефолте #ifndef, без ошибки и без лога.
	const ShaderDefines kVariantDefines = {
		// Включает САМО переключение (чтение буферов состояний). Без него пролог собирается
		// без них и показывает дефолт слота — так живут пользовательские surface из кода игры,
		// которым эти буферы никто не биндит.
		{ "TEXTURE_VARIANTS",    "1" },
		{ "MAX_VARIATIVE_SLOTS", std::to_string(MAX_VARIATIVE_SLOTS) },
		{ "MAX_SLOTS",           std::to_string(MAX_SLOTS) },
		{ "MAX_UVL_BLOCKS",      std::to_string(MAX_UVL_BLOCKS) },
	};
	engine_context->CreateFragmentShader("main_surface_fs",        "../engine/shaders_code/main_pass/surface.hlsl", /*dont_save=*/true, kVariantDefines);
	engine_context->CreateFragmentShader("untextured_surface_fs",  "../engine/shaders_code/main_pass/untextured/surface.hlsl", /*dont_save=*/true);
	engine_context->CreateFragmentShader("transparent_surface_fs", "../engine/shaders_code/transparent_pass/surface.hlsl", /*dont_save=*/true, kVariantDefines);
	engine_context->CreateFragmentShader("shadow_fs",              "../engine/shaders_code/shadow_pass/shadow_pass.frag.hlsl", /*dont_save=*/true);
	engine_context->CreateFragmentShader("skybox_fs",              "../engine/shaders_code/skybox/skybox.frag.hlsl", /*dont_save=*/true);
	engine_context->CreateFragmentShader("debug_collider_fs",      "../engine/shaders_code/debug/debug_collider.frag.hlsl", /*dont_save=*/true);

	// ── Compute-ШЕЙДЕРЫ (не программы). Программы (csp) держат указатели на буферы/атласы и
	//    создаются игрой (DefaultShaderProgramSet::Set*Programs); сюда идут только сами CSD,
	//    на которые те ссылаются по имени. ──
	engine_context->CreateComputeShader("bloom_prefilter_cs", "../engine/shaders_code/comp/bloom_prefilter.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("bloom_down_cs",      "../engine/shaders_code/comp/bloom_down.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("bloom_up_cs",        "../engine/shaders_code/comp/bloom_up.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("bloom_composite_cs", "../engine/shaders_code/comp/bloom_composite.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("ssao_cs",            "../engine/shaders_code/comp/ssao.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("ssao_blur_h_cs",     "../engine/shaders_code/comp/ssao_blur_h.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("ssao_blur_v_cs",     "../engine/shaders_code/comp/ssao_blur_v.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("ao_composite_cs",    "../engine/shaders_code/comp/ao_composite.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("fog_cs",             "../engine/shaders_code/comp/fog.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("culling_clear_cs",   "../engine/shaders_code/comp/culling_clear.comp.hlsl", /*dont_save=*/true);
	engine_context->CreateComputeShader("culling_pib_cs",     "../engine/shaders_code/comp/culling_pib.comp.hlsl", /*dont_save=*/true);

	// ── Render-программы. Имена — короткие, в стиле URP: их видно в списке шейдеров редактора
	//    и в materials.json, читаются они чаще, чем пишутся. Подчёркивание = служебная программа,
	//    которую не выбирают руками (движковое соглашение: _FallbackAtlas, _staging, _cameraBuffer).
	//    "LitColor" — тот же свет и тот же PBR, что у Lit, но БЕЗ карт: цвет берётся из params
	//    материала. Именно "Lit", а не "Unlit": освещение здесь считается полностью. ──
	{
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry();
		// Оба буфера вариантов — во ФРАГМЕНТНОМ списке, и это не вкусовщина: вершинник
		// main_pass_vs общий не только с LitColor/LitTransparent, но и с программами ИГР
		// (фрактальные поверхности mygame). Буфер в вершинном списке обязана была бы биндить
		// КАЖДАЯ такая sp — иначе «Missing vertex storage buffer binding». Поэтому вершинник
		// отдаёт лишь row (он у него и так есть), а префикс читает фрагментник — и платят за
		// это только те sp, которым варианты нужны.
		engine_context->CreateShaderProgram("Lit", spd, RP::MAIN_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"main_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo, TextureSlotRole::Normal, TextureSlotRole::ORM, TextureSlotRole::Emissive },
			/*dont_save=*/true);

		// Тот же vs и те же буферы, но fs без текстур: материал без карт рисуется цветом из params.
		// Без буферов вариантов вовсе: у текстурелесс материала их нет по определению.
		engine_context->CreateShaderProgram("LitColor", spd, RP::MAIN_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"untextured_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ }, /*dont_save=*/true);
	}
	{
		// Прозрачные: глубину читают, но НЕ пишут (иначе перекрывали бы друг друга), блендинг включён.
		// Из света берут только _lightBuffer — теневые карты прозрачные не читают.
		ShaderProgramDescription spd;
		spd.BehavesAsTransparentGeometry();
		engine_context->CreateShaderProgram("LitTransparent", spd, RP::TRANSPARENT_PASS,
			"main_pass_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER },
			"transparent_surface_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo, TextureSlotRole::Normal }, /*dont_save=*/true);
	}
	{
		// Теневой: камера СВЕТОВАЯ (DefaultLightCameraBuffer вместо _cameraBuffer), цвета нет.
		ShaderProgramDescription spd;
		spd.BehavesAsShadowCaster();
		engine_context->CreateShaderProgram("ShadowCaster", spd, RP::SHADOW_PASS,
			"shadow_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"shadow_fs", { }, { }, /*dont_save=*/true);
	}
	{
		// Каркас коллайдеров: линии поверх картинки, глубина не участвует вовсе.
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->IgnoresDepth()->AsLineList();
		engine_context->CreateShaderProgram("Wireframe", spd, RP::DEBUG_PASS,
			"debug_collider_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER },
			"debug_collider_fs", { }, { }, /*dont_save=*/true);
	}
	{
		// Скайбокс: transformless (без Positions, PIB=-1) — из буферов ему нужна только камера.
		// z=w в вершиннике даёт глубину РОВНО на клире, поэтому LESS не пройдёт — нужен LESS_OR_EQUAL.
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->ReadsDepthOnly()->WithDepthCompare(SDL_GPU_COMPAREOP_LESS_OR_EQUAL);
		engine_context->CreateShaderProgram("Skybox", spd, RP::MAIN_PASS,
			"skybox_vs", { DEFAULT_CAMERA_BUFFER },
			"skybox_fs", { }, { }, /*dont_save=*/true);
	}

	{
		// UI-оверлей: рисует энтити, которые emit-ит UI_Yoga. Программа движковая — раньше жила
		// в игре (DefaultShaderProgramSet::SetUIProgram), хотя сам UI_Yoga давно подсистема движка,
		// и без неё UI не рисовался бы вообще. VS тянет POSITION+UV (юнит-квад), матрица даёт NDC;
		// FS — заливка albedo (без света) + текст. Объявление FS-буферов здесь = их usage, по
		// которому BakePending эти буферы и создаёт. Слот Albedo = фон узла.
		engine_context->CreateVertexShader("ui_vs", "../engine/shaders_code/ui/ui.vert.hlsl",
			POS_UV_NORM_POOL, { POSITION, UV }, /*dont_save=*/true);
		engine_context->CreateFragmentShader("ui_fs", "../engine/shaders_code/ui/ui.frag.hlsl", /*dont_save=*/true, kVariantDefines);

		ShaderProgramDescription spd;
		spd.BehavesAsUIOverlay();
		engine_context->CreateShaderProgram("UI", spd, RP::UI_PASS,
			"ui_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_INSTANCE_BUFFER },
			// Буферы вариантов — в ХВОСТ фрагментного списка (t6..t8 после GlyphUVL t5). Вершинный
			// не трогаем: ui_vs и так отдаёт row, а буфер в его списке пришлось бы биндить всем.
			"ui_fs", { UI_TEXT_RANK_BUFFER, UI_TEXT_INDEX_BUFFER, UI_TEXT_BUFFER, UI_FONT_UVL_BUFFER,
			           DEFAULT_TEX_STATE_RANK_BUFFER, DEFAULT_TEX_STATE_INDEX_BUFFER, DEFAULT_TEX_STATE_BUFFER },
			{ TextureSlotRole::Albedo }, /*dont_save=*/true);
	}

	// ── Push-константы движковых sp: код-байндинг живёт рядом с созданием программы. Реестр
	//    ShaderManager вешает его по имени — и здесь, и на любой будущей пересборке этой sp. ──
	shader_manager->CreatePushFunc<RP::ShadowPushData>("ShadowCaster",
		[](const PushConstantBinder& b, RP::ShadowPushData data) { b.PushFragment(data); });   // слот 0
	shader_manager->CreatePushFunc<RP::DebugColliderPushData>("Wireframe",
		[](const PushConstantBinder& b, RP::DebugColliderPushData data) { b.PushFragment(data); });   // fragment slot 0 -> b0, space3

	// Счётчик источников света — ПЕРВЫЙ fragment-uniform (b0, space3) у каждой программы, чей fs
	// включает лайтинг-базу: UVL/params/раскладка съезжают за ним сами (uvl_slot =
	// binder.frag_count, см. RenderManager). Игровые программы на движковой базе регистрируют
	// такой же пуш у себя (GameShaderSet / FractalShaderSet) — иначе их униформы уедут на слот ниже.
	for (const char* lit_sp : { "Lit", "LitColor", "LitTransparent", "_Fallback" })
		shader_manager->CreatePushFunc<RP::LightCountPushData>(lit_sp,
			[](const PushConstantBinder& b, RP::LightCountPushData data) { b.PushFragment(data); });
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
		_SetDefaultCommonResources(engine_context, safe_f_u32(GetWidth()), safe_f_u32(GetHeight()));
		SetDefaultCullingPass(engine_context);     // GPU-каллинг: out_pib до SHADOW_PASS (индекс 5)
		SetDefaultShadowPCFRenderPass(engine_context, light_data_module);
		SetDefaultMainRenderPass(engine_context, light_data_module);
		SetDefaultAOPass(engine_context);           // SSAO по глубине main'а, применяется до тумана
		//SetDefaultFogPass(engine_context);          // атмосфера по глубине main'а: ПОСЛЕ AO, до прозрачных
		SetTransparentPass(engine_context, light_data_module);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);       // bloom от эмиссии (compute) + composite/tonemap в scene_hdr
		SetUIPass(engine_context);                 // UI-оверлей (NDC-квады) в scene_hdr после bloom, до present
		SetPresentPass(engine_context);            // финал: HDR-сцену в свопчейн (blit)
	}
}

void Engine::InitUICommands()
{
	// Регистрация билтин UI-команд вынесена в DefaultCommandSet (свободные функции,
	// как DefaultUpdateSet/DefaultRenderPassSet) — лямбды stateless, полей Engine не касаются.
	DefaultCommandSet::SetAll(*input_manager);
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
				OnWindowResized(event.window.data1, event.window.data2, 0, 0);

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

	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();

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
	delete pib_data_module;
	delete transform_data_module;
	delete light_data_module;
	delete ui_data_module;
	delete tex_state_data_module;
	delete ui_yoga;   // YGNodeFreeRecursive дерева + YGConfigFree (в его dtor)

	// Платформу подняли мы (InitPlatform) — мы же её и рушим. Строго после менеджеров и ImGui:
	// они держат ресурсы устройства, а release окна должен опережать уничтожение девайса.
	SDL_ReleaseWindowFromGPUDevice(dev, win);
	SDL_DestroyGPUDevice(dev);
	SDL_DestroyWindow(win);
	SDL_Quit();

	dev = nullptr;
	win = nullptr;
}
