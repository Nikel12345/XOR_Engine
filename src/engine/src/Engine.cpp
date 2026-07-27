#include "PCH.h"
#include "Engine.h"
// Engine.h теперь только forward-декларации — полные типы менеджеров тянет этот TU.
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
#include "FontManager.h"   // new FontManager() в ctor
#include "TextureLoader.h"
#include "BatchBuilder.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "InstanceDataModule.h"
#include "LightDataModule.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"
#include "UI_DataModule.h"
#include "UI_Yoga.h"   // new UI_Yoga() в ctor + Emit (flex-раскладка UI → энтити)
#include "EngineContext.h"
#include "DefaultUpdateSet.h"
#include "DefaultRenderPassSet.h"
#include "TexturesPresets.h"
#include "ComponentSerializer.h"
#include "MaterialParamsSpec.h"       // RegisterBuiltinMaterialParamsSpecs — схемы типов params
#include "PositionStructure.h"        // FMT_PosUVNormal — раскладка вершин для fallback-sp
#include "DefaultCommandSet.h"        // регистрация билтин UI-команд (вынесено из класса)
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

using namespace ShaderBase;   // POSITION/UV/... в раскладке fallback-vs

// ============================================================
//  Engine: конструирование/разрушение + инициализация дефолтов.
//  Кадровый конвейер — Engine_Frame.cpp; save/load сцены — Engine_Scene.cpp;
//  регистрация UI-команд — DefaultCommandSet.cpp.
// ============================================================

void Engine::OnWindowResized(Sint32 w, Sint32 h)
{
	width = safe_sint32_f(w);
	height = safe_sint32_f(h);

	// UI-раскладка зависит от размера кадра (px→NDC) → пересчитать на следующем Emit.
	if (ui_yoga) ui_yoga->MarkDirty();

	// GPU-таргеты (depth + HDR) ресайзятся НЕ здесь, а в RenderFunc по размеру реально полученного
	// свопчейна — раз на отрисованный кадр. Событие resized летит сотнями за drag; пересоздавать
	// большие текстуры на каждое = VRAM-захлёб и DEVICE_LOST. Здесь только обновляем width/height
	// (для aspect-ratio камеры и пр.).
}

Engine::Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height)
{
	this->win = window;
	this->dev = dev;
	this->width = width;
	this->height = height;
	transfer_manager = new TransferManager(dev);
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
			{ GeometryStreams::VERTEX_POS_BUFFER, GeometryStreams::VERTEX_UV_BUFFER, GeometryStreams::VERTEX_NORMTAN_BUFFER },
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
	engine_context->CreateModel("quad", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
		v = {
			{ 0,0,0,  0,0,  0,0,1,  1,0,0 },
			{ 1,0,0,  1,0,  0,0,1,  1,0,0 },
			{ 1,1,0,  1,1,  0,0,1,  1,0,0 },
			{ 0,1,0,  0,1,  0,0,1,  1,0,0 },
		};
		i = { 0, 1, 2, 0, 2, 3 };
	}, AnchorShift::Keep, /*dont_save=*/true);

	engine_context->CreateModel("sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
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
				vert.u = (float)j / (float)slices;
				vert.v = (float)i / (float)stacks;
				vert.nx = nx; vert.ny = ny; vert.nz = nz;
				vert.tx = -st; vert.ty = 0.0f; vert.tz = ct;        // касательная = ∂pos/∂θ
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
	SetDefaultVertexUpdater(*engine_context);
	SetDefaultIndexUpdater(*engine_context);
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
		_SetDefaultCommonResources(engine_context, safe_f_u32(width), safe_f_u32(height));
		SetDefaultCullingPass(engine_context);     // GPU-каллинг: out_pib до SHADOW_PASS (индекс 5)
		SetDefaultShadowPCFRenderPass(engine_context, light_data_module);
		SetDefaultMainRenderPass(engine_context);
		SetTransparentPass(engine_context);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);       // bloom от эмиссии (compute) + composite/tonemap в scene_hdr
		SetUIPass(engine_context);                 // UI-оверлей (NDC-квады) в scene_hdr после bloom, до present
		SetPresentPass(engine_context);            // финал: HDR-сцену в свопчейн (blit)
	}
	//SetDefaultShadowVSMRenderPass(pass_manager, texture_manager, buffer_manager, object_manager, batch_builder);
	//SetDefaultShadowBlurPass(pass_manager, buffer_manager); // ДЛЯ VSM
	//SetDefaultMainRenderPass(pass_manager, texture_manager, buffer_manager);
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
