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
#include "MaterialParamsSpec.h"
#include "PositionStructure.h"
#include "DefaultCommandSet.h"
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

Engine::Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height)
{
	this->win = window;
	this->dev = dev;
	// Стартовый размер (аргументы ctor) кладём ТОЛЬКО в size_state_ — отдельных width/height в движке нет,
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

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = dev;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
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
	// Бейк GPU-ресурсов здесь НЕ делаем: игра объявляет свои ресурсы и шейдерные программы позже
	// (атласы в Game::Init, sp — в манифесте сцены), а именно объявления sp несут usage-флаги.
	// Точка бейка — конец первого Engine::LoadScene.
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

	// Fallback-sp: материал с УДАЛЁННОЙ sp рисуется им (аналог textureless — цвет из params, без
	// текстур). Буферы (InitDefaultBufferUpdaters) и MAIN_PASS (InitPasses) к этому моменту готовы.
	// dont_save=true у всей тройки vs/fs/sp — движковая инфраструктура, в shaders.json не идёт.
	{
		using namespace DefaultBuffersNames;
		engine_context->CreateVertexShader("_fallback_vs",
			"../engine/shaders_code/main_pass/main_pass.vert.hlsl",
			POS_UV_NORM_POOL,
			{ ShaderBase::POSITION, ShaderBase::UV, ShaderBase::NORMAL, ShaderBase::TANGENT },
			/*dont_save=*/true);
		engine_context->CreateFragmentShader("_fallback_fs", "../engine/shaders_code/main_pass/untextured/surface.hlsl", /*dont_save=*/true);
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->DoesNotCull();
		engine_context->CreateShaderProgram("_FallbackShader", spd, DefaultRenderPassNamespace::MAIN_PASS,
			"_fallback_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"_fallback_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ }, /*dont_save=*/true);   // текстур нет
		batch_builder->SetFallbackShader("_FallbackShader");   // ПО ИМЕНИ: удаление fallback → промах → пустой рендер
	}

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
	// Классический скайбокс — src/game/saved_scene, фрактал — src/mygame/saved_scene_fractal.
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

	// UI-текст: bits/wordbase/index/text (UI_DataModule) + GlyphUVL (FontManager, шрифт "default").
	// Буферы бейкаются, когда sp_ui объявит их usage (создаётся в Game::MainInit до старта потоков).
	SetUITextUpdaters(*engine_context, ui_data_module, font_manager, "default");
}

void Engine::InitPasses()
{
	using namespace DefaultRenderPassNamespace;

	{
		_SetDefaultCommonResources(engine_context, safe_f_u32(GetWidth()), safe_f_u32(GetHeight()));
		SetDefaultCullingPass(engine_context);     // GPU-каллинг: out_pib до SHADOW_PASS (индекс 5)
		SetDefaultShadowPCFRenderPass(engine_context, light_data_module);
		SetDefaultMainRenderPass(engine_context);
		SetTransparentPass(engine_context);
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


Engine::~Engine()
{
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
	delete slot_controller;
	delete thread_controller;
	delete material_manager;
	delete input_manager;
	delete texture_loader;
	delete font_manager;   // dtor: TTF_CloseFont всех шрифтов + TTF_Quit
	delete pib_data_module;
	delete transform_data_module;
	delete light_data_module;
	delete ui_data_module;
	delete ui_yoga;   // YGNodeFreeRecursive дерева + YGConfigFree (в его dtor)

	dev = nullptr;
	win = nullptr;
}
