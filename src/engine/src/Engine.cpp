#include "PCH.h"
#include "Engine.h"
#include "TexturesPresets.h"
#include "ComponentSerializer.h"
#include "MaterialParams.h"           // Opaque/TransparentMaterialParams — билтин-типы params
#include "MaterialParamsRegistry.h"   // реестр типов params (дропдаун Kind)
#include "PositionStructure.h"        // FMT_PosUVNormal — раскладка вершин для fallback-sp
#include "DefaultCommandSet.h"        // регистрация билтин UI-команд (вынесено из класса)
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

// ============================================================
//  Engine: конструирование/разрушение + инициализация дефолтов.
//  Кадровый конвейер — Engine_Frame.cpp; save/load сцены — Engine_Scene.cpp;
//  регистрация UI-команд — DefaultCommandSet.cpp.
// ============================================================

void Engine::OnWindowResized(Sint32 w, Sint32 h)
{
	width = safe_sint32_f(w);
	height = safe_sint32_f(h);

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

	batch_builder = new BatchBuilder();

	pib_data_module = new PIB_DataModule();
	transform_data_module = new TransformDataModule();
	instance_data_module = new InstanceDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	bound_sphere_data_module = new BoundSphereDataModule();

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	engine_context->SetEngine(this);   // делегирование Save/LoadScene (оркестрация сцены-папки)
	InitDefaultBufferUpdaters();
	InitPasses();
	InitUICommands();
	RegisterBuiltinComponentSerializers();   // сериалайзеры компонентов для save/load сцены
	InitDefaultMaterialParams();             // билтин-типы params для UI-дропдауна Kind
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
	TextureHandle* dummy = engine_context->CreateTextureFromFile("_NoTextureDummy", "_FallbackAtlas", "../engine/textures/dummy.png",
		ChannelConvention::AsIs, /*dont_save=*/true);   // движковый дефолт — в файл сцены не идёт

	batch_builder->SetDummyTexture(dummy);
	InitDefaultResources();
}

// Билтин-типы params для UI-дропдауна Kind. Пользователь дорегистрирует свои из кода игры
// (MaterialParamsRegistry::Get().Register{...}) — движок трогать не надо.
void Engine::InitDefaultMaterialParams()
{
	auto& r = MaterialParamsRegistry::Get();
	// None: applyDefault чистит блоб; edit пустой (нечего рисовать).
	r.Register({ MaterialParamsKind::None, "None",
		[](EngineContext*, Material* m){ m->params.clear(); m->params_kind = MaterialParamsKind::None; } });
	r.Register({ MaterialParamsKind::Opaque, "Opaque",
		[](EngineContext* ctx, Material* m){ ctx->SetMaterialParams(m, OpaqueMaterialParams{}); },
		[](Material* m){
			if (m->params.size() < sizeof(OpaqueMaterialParams)) return;   // защита от рассинхрона kind/блоба
			auto* p = reinterpret_cast<OpaqueMaterialParams*>(m->params.data());
			ImGui::ColorEdit3("Base Color", p->baseColor);
			ImGui::ColorEdit3("Emissive", p->emissive);
			ImGui::SliderFloat("Emissive Strength", &p->emissiveStrength, 0.0f, 8.0f);
			ImGui::SliderFloat("Metallic", &p->metallic, 0.0f, 1.0f);
			ImGui::SliderFloat("Roughness", &p->roughness, 0.0f, 1.0f);
		} });
	r.Register({ MaterialParamsKind::Transparent, "Transparent",
		[](EngineContext* ctx, Material* m){ ctx->SetMaterialParams(m, TransparentMaterialParams{}); },
		[](Material* m){
			if (m->params.size() < sizeof(TransparentMaterialParams)) return;
			auto* p = reinterpret_cast<TransparentMaterialParams*>(m->params.data());
			ImGui::SliderFloat("Alpha", &p->alpha, 0.0f, 1.0f);
		} });
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
	{
		using namespace DefaultBuffersNames;
		engine_context->CreateVertexShader("_fallback_vs",
			"../engine/shaders_code/main_pass/main_pass.vert.hlsl",
			{ { &FMT_PosUVNormal, { POSITION, UV, NORMAL, TANGENT } } });
		engine_context->CreateFragmentShader("_fallback_fs", "../engine/shaders_code/main_pass/untextured/surface.hlsl");
		ShaderProgramDescription spd;
		spd.BehavesAsOpaqueGeometry()->DoesNotCull();
		engine_context->CreateShaderProgram("_FallbackShader", spd, DefaultRenderPassNamespace::MAIN_PASS,
			"_fallback_vs", { DEFAULT_TRANSFORM_BUFFER, DEFAULT_OUT_PIB_BUFFER, DEFAULT_CAMERA_BUFFER, DEFAULT_INSTANCE_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER },
			"_fallback_fs", { DEFAULT_LIGHT_BUFFER, DEFAULT_LIGHT_CAMERA_BUFFER, DEFAULT_CAMERA_BUFFER },
			{ });   // текстур нет
		batch_builder->SetFallbackShader("_FallbackShader");   // ПО ИМЕНИ: удаление fallback → промах → пустой рендер
	}
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
	delete pib_data_module;
	delete transform_data_module;
	delete light_data_module;

	dev = nullptr;
	win = nullptr;
}
