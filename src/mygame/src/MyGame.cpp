#include "PCH.h"
#include "MyGame.h"
#include "imgui.h"
#include "TexturesPresets.h"
#include "DefaultShaderSet.h"
#include "MaterialParams.h"        // OpaqueMaterialParams + раскладки факторов

// ===================== MyGame: хост приложения ==============================

MyGame::MyGame(Engine* engine)
{
    this->engine = engine;

    textureManager = engine->GetTextureManager();
    modelManager   = engine->GetModelManager();
    objectManager  = engine->GetObjectManager();
    cameraManager  = engine->GetCameraManager();

    threadController = engine->GetThreadController();
    input            = engine->GetInputManager();

    width  = engine->GetWidth();
    height = engine->GetHeight();

    ctx = engine->GetEngineContext();

    // Сервисы, которые получит каждый режим: примитивы движка + общая шина.
    mode_ctx = ModeContext{ engine, ctx, objectManager, cameraManager, input, &bus, width, height };
}

SDL_AppResult MyGame::MainInit()
{
    // Камера общая для приложения; вид/позицию подстраивает каждый режим в Enter.
    cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);

    // Стартуем с меню. Дальше переходы — через ChangeMode из логики режима.
    ChangeMode(std::make_unique<MainMenuMode>(mode_ctx));

    return SDL_APP_CONTINUE;
}

void MyGame::ChangeMode(std::unique_ptr<IGameMode> next)
{
    if (current) current->Exit();
    bus.ClearHandlers();              // подписки прошлого режима снимаем; новый переподпишет своё
    current = std::move(next);
    if (current) current->Enter();
}

SDL_AppResult MyGame::MainIterate()
{
    // TODO: реальный кадровый тайминг. Пока фиксированный шаг под targetUPS.
    const float dt = 1.0f / 60.0f;

    // ----- Общий ввод/камера: одинаков для всех режимов, поэтому в хосте -----
    input->DrainKeyEvents(key_events_scratch);
    for (const InputManager::KeyEvent& e : key_events_scratch) {
        if (!e.down) continue;
        switch (e.scancode) {
        case SDL_SCANCODE_ESCAPE:
            // выход / возврат в меню
            break;
        default:
            break;
        }
    }

    Camera* camera = cameraManager->GetActiveCamera();
    ImGuiIO& io = ImGui::GetIO();

    float wheel = input->ConsumeWheelDelta();
    if (wheel != 0.0f && !io.WantCaptureMouse) {
        camera->SpeedChange(wheel * 0.5f);
    }
    mouse_x = input->MouseX();
    mouse_y = input->MouseY();
    //camera->RotateView(mouse_x, mouse_y, input->IsMouseButtonDown(SDL_BUTTON_LEFT));

    input->ExecuteCommands(ctx);

    // ----- Логика активного режима + разбор накопленных событий --------------
    if (current) current->Update(dt);
    bus.Dispatch();

    return SDL_APP_CONTINUE;
}

// ===================== MainMenuMode ========================================

void MainMenuMode::Enter()
{
    s_.objectManager->CreateScene("main_menu");
    s_.ctx->SetActiveScene("main_menu");

    EngineContext* ctx = s_.ctx;
	TextureAtlas* atlas = ctx->CreateTextureAtlas("albedo_atlas", TexturePresets::GetCreateInfo(TexturePreset::Albedo_Atlas4096_3Layer), DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* NAOPBR_atlas = ctx->CreateTextureAtlas("NAOPBR_atlas", TexturePresets::GetCreateInfo(TexturePreset::NAOPBR_Atlas4096_3Layer), DefaultSamplersNames::DEFAULT_SAMPLER);
    
    {
        using namespace DefaultShaderProgramSet;
        SetMainShaderProgram(ctx);
        SetDefaultShadowShaderProgram(ctx);
        SetTransparentShaderProgram(ctx);
        SetDebugColliderProgram(ctx);
        SetBloomPrograms(ctx);   // программы bloom под BLOOM_PASS (проход создаёт engine)
        SetCullingPibPrograms(ctx, s_.engine->GetLightDataModule());   // GPU-каллинг: out_pib
    }

    ModelData* quad = ctx->CreateModel("quad", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
        v = {
            { 0,0,0,  0,1,  0,0,1,  1,0,0 },
            { 1,0,0,  1,1,  0,0,1,  1,0,0 },
            { 1,1,0,  1,0,  0,0,1,  1,0,0 },
            { 0,1,0,  0,0,  0,0,1,  1,0,0 },
        };
        i = { 0, 1, 2, 0, 2, 3 };
    }, AnchorShift::Center);

    TextureHandle* texture_cube = ctx->CreateTextureFromFile("albedo_cube", "albedo_atlas", "textures/level_1_orig.png");

    auto player_model = ctx->CreateModel("player_model", "models/space_craft.bin", "models/space_craft_i.bin");
    auto player_texture = ctx->CreateTextureFromFile("player_texture", "albedo_atlas", "textures/1.png");

	auto enemy_model = ctx->CreateModel("enemy_model", "models/inter/inter.bin", "models/inter/inter_i.bin");

	auto enemy_texture_color = ctx->CreateTextureFromFile("enemy_texture_color", "albedo_atlas", "textures/inter/inter_color.jpg");
    auto enemy_texture_eml = ctx->CreateTextureFromFile("enemy_texture_eml", "NAOPBR_atlas", "textures/inter/inter_eml.jpg");
	auto enemy_texture_metal = ctx->CreateTextureFromFile("enemy_texture_metal", "NAOPBR_atlas", "textures/inter/inter_metal.jpg");



	auto common_normals = ctx->CreateTextureFromFile("norm", "NAOPBR_atlas", "textures/car_norm.png");

	// "sp" теперь требует 4 слота (albedo, normal, orm, emissive). Для материалов без реальных
	// ORM/эмиссии — искусственные дефолт-белые 2×2 (множитель-единица: AO=1, factor-driven metal/rough/emission).
	ctx->GetTextureManager()->CreateTexture("default_orm",      "NAOPBR_atlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));
	ctx->GetTextureManager()->CreateTexture("default_emissive", "NAOPBR_atlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));



    auto material_player = ctx->CreateMaterial("car", {
    {TextureSlotRole::Albedo, "player_texture"},
    {TextureSlotRole::Normal, "norm"},
    {TextureSlotRole::ORM, "default_orm"},
    {TextureSlotRole::Emissive, "default_emissive"} },
        { "sp", "sp_shadow" });

    //auto material_enemy = ctx->CreateMaterial("enemy",
    //    { TextureSlotRole::Albedo, "enemy_texture_color" },
    //    { TextureSlotRole::Normal, "norm" },
    //    { TextureSlotRole::Emissive, "enemy_texture_eml" },
    //    { TextureSlotRole::MetallicRoughness, "enemy_texture_metal" }
    //    { "sp", "sp_shadow" });
    auto material_enemy = ctx->CreateMaterial("material_enemy", {
        {TextureSlotRole::Albedo, "enemy_texture_color"},
        {TextureSlotRole::Normal, "norm"},
        {TextureSlotRole::Emissive, "enemy_texture_eml"},
        {TextureSlotRole::MetallicRoughness, "enemy_texture_metal"} },
        { "sp", "sp_shadow" });

	float scale = 0.03f; // масштаб модели игрока
    ctx->CreateEntity("main_menu", DrawComponent{}, MaterialComponent{ {material_player, material_player, material_player} }, ModelComponent{ player_model },
        PositionProxy16{ 
            1 * scale, 0, 0, 0,  
            0, 1 * scale, 0, -1.5f,  
            0, 0, 1 * scale, 0.2f,  
            0, 0, 0, 1 },
        ShadowComponent{}
    );
    ctx->CreateEntity("main_menu", DrawComponent{}, MaterialComponent{ {material_enemy, material_enemy, material_enemy} }, ModelComponent{ enemy_model },
        PositionProxy16{ 
            1 * scale, 0, 0, 0,  
            0, -1 * scale, 0, -1.5f,  
            0, 0, 1 * scale, 0.2f,  
            0, 0, 0, 1 },
        ShadowComponent{}
		);

    auto material_sprite = ctx->CreateMaterial("sprite", {
    {TextureSlotRole::Albedo, "albedo_cube"},
    {TextureSlotRole::Normal, "norm"},
    {TextureSlotRole::ORM, "default_orm"},
    {TextureSlotRole::Emissive, "default_emissive"} },
        { "sp", "sp_shadow" });

    // Opaque-материалы несут params (дефолт-белый baseColorFactor = без тинта): иначе их
    // MaterialBlock @ b1 остался бы несвязанным. metallic/roughness/emission — задел.
    ctx->SetMaterialParams(material_player, OpaqueMaterialParams{});
    ctx->SetMaterialParams(material_enemy,  OpaqueMaterialParams{});
    ctx->SetMaterialParams(material_sprite, OpaqueMaterialParams{});

    Camera* cam = s_.cameraManager->GetActiveCamera();
    const glm::vec3 camPos = cam->GetPosition();
    const glm::vec3 fwd    = cam->GetForward();
    const float d = glm::dot(glm::vec3(0.0f) - camPos, fwd);   // = 5 для дефолтной камеры (0,0,5)→ -Z

    // proj[1][1] = 1/tan(fovy/2), proj[0][0] = 1/(aspect·tan(fovy/2)) → полу-область = d / элемент.
    const glm::mat4 proj = cam->GetProj();
    const float halfW = d / proj[0][0];
    const float halfH = d / proj[1][1];
    ctx->CreateEntity("main_menu",
        MaterialComponent{ { material_sprite } },
        ModelComponent{ quad },
        PositionProxy16{
            2.0f * halfW, 0,          0, 0.0f,
            0,           2.0f * halfH, 0, 0.0f,
            0,           0,           1, 0.0f,
            0,           0,           0, 1.0f
        },
        DrawComponent{}
    );
    // TODO: ресурсы/сущности меню через s_.ctx->Create*.
    // У меню нет зон/таймеров врагов — значит и событий этих здесь просто нет.
}

void MainMenuMode::Update(float /*dt*/)
{
    // TODO: UI меню. Переход в игру — например по кнопке:
    //   game->ChangeMode(std::make_unique<GameplayMode>(...));
    // (для перехода режиму нужен путь к хосту — добавишь, когда понадобится:
    //  колбэк в ModeContext или указатель на MyGame.)
}

// ===================== GameplayMode ========================================

void GameplayMode::Enter()
{
    s_.objectManager->CreateScene("gameplay");
    s_.ctx->SetActiveScene("gameplay");

    // Триггеры — ТОЛЬКО этой сцены. Пример: лимит времени на уровень.
    timers_.clear();
    timers_.push_back(Timer{ 1, 5.0f });

    // Реакции — тоже локальные. Подписки снимутся в ChangeMode (ClearHandlers).
    s_.bus->Subscribe<TimerExpired>([](const TimerExpired& e) {
        SDL_Log("Timer %d expired", e.timer_id);
    });
    s_.bus->Subscribe<EnemyKilled>([](const EnemyKilled& e) {
        SDL_Log("Enemy %u killed", e.enemy);
    });

    // TODO: ресурсы/сущности геймплея, зоны (ReachedZone) и т.п.
}

void GameplayMode::Update(float dt)
{
    EventBus& bus = *s_.bus;

    // Производитель-опрос: таймеры. Пересёк 0 → одно событие (edge, не каждый кадр).
    for (Timer& t : timers_) {
        if (t.done) continue;
        t.remaining -= dt;
        if (t.remaining <= 0.0f) { t.done = true; bus.Emit(TimerExpired{ t.id }); }
    }

    // TODO: зоны (проверка дистанции через ForEach → Emit(ReachedZone));
    //       смерть врагов — собрать в dead_scratch_, удалить после обхода,
    //       и на каждого Emit(EnemyKilled{e}).
}

void GameplayMode::Exit()
{
    // Сцену можно деактивировать/очистить, если уходим из геймплея насовсем.
    // s_.objectManager->SetSceneState("gameplay", false);
}
