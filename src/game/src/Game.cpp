#include "PCH.h"
#include "Game.h"
#include "TexturesPresets.h"
#include "DefaultShaderSet.h"

Game::Game(Engine* engine)
{
    game_state = GameState::MAIN_MENU;
    this->engine = engine;
    this->game_state = GameState::MAIN_MENU;

	textureManager = engine->GetTextureManager();
	modelManager = engine->GetModelManager();
	objectManager = engine->GetObjectManager();
	cameraManager = engine->GetCameraManager();

	threadController = engine->GetThreadController();
	input = engine->GetInputManager();



	width = engine->GetWidth();
	height = engine->GetHeight();

	ctx = engine->GetEngineContext();
}

SDL_AppResult Game::MainInit()
{
    objectManager->CreateScene("main_menu");
    Camera* camera = cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);

    camera->SetView(
        glm::vec3(2.0f, 0.7f, 3.5f), // позиция камеры
        glm::vec3(0.43f, -0.4f, -0.8f), // точка взгляда
        glm::vec3(0.0f, 1.0f, 0.0f)  // вектор вверх
    );

    TextureAtlas* atlas = ctx->CreateTextureAtlas("albedo_atlas", TexturePresets::GetCreateInfo(TexturePreset::Albedo_Atlas2048_1Layer), DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* NAOPBR_atlas = ctx->CreateTextureAtlas("NAOPBR_atlas", TexturePresets::GetCreateInfo(TexturePreset::NAOPBR_Atlas2048_1Layer), DefaultSamplersNames::DEFAULT_SAMPLER);

	TextureHandle* texture_cube = ctx->CreateTextureFromFile("albedo_cube", "albedo_atlas", "textures/assets/cube_test.png");
	TextureHandle* norm = ctx->CreateTextureFromFile("norm", "NAOPBR_atlas", "textures/assets/car_norm.png");

    TextureHandle* texture_car = ctx->CreateTextureFromFile("new_car", "albedo_atlas", "textures/assets/new_car.png");
	TextureHandle* ground = ctx->CreateTextureFromFile("new_car_ground", "albedo_atlas", "textures/assets/new_car_ground.png");
	TextureHandle* glass = ctx->CreateTextureFromFile("new_car_glass", "albedo_atlas", "textures/assets/new_car_glass.png");

    {
        using namespace DefaultShaderProgramSet;
        SetMainShaderProgram(ctx);
        SetDefaultShadowShaderProgram(ctx);
    }
    
    auto material_car = ctx->CreateMaterial("car", {
        {TextureSlotRole::Albedo, "new_car"},
        {TextureSlotRole::Normal, "norm"} },
        { "sp", "sp_shadow" });
	auto material_car2 = ctx->CreateMaterial("car2", {
        {TextureSlotRole::Albedo, "new_car"},
        {TextureSlotRole::Normal, "norm"} },
		{ "sp", "sp_shadow" });
    auto material_ground = ctx->CreateMaterial("ground", {
        {TextureSlotRole::Albedo, "new_car_ground"},
        {TextureSlotRole::Normal, "norm"} },
		{ "sp", "sp_shadow" });
    auto material_glass = ctx->CreateMaterial("glass", {
        {TextureSlotRole::Albedo, "new_car_glass"},
        {TextureSlotRole::Normal, "norm"} },
		{ "sp", "sp_shadow" });

    ModelData* model_car = ctx->CreateModel("car", "models/new_car_n_fixed.bin", "models/new_car_n_fixed_i.bin");

    ctx->CreateEntity("main_menu",
        MaterialComponent{ {material_car, material_car2, material_glass, material_ground} },
        ModelComponent{ model_car },
        PositionProxy16{ 1,0,0,3,  0,1,0,0,  0,0,1,0,  0,0,0,1 },
        ShadowComponent{}
    );
    ctx->CreateEntity("main_menu",
        MaterialComponent{ {material_car, material_car2, material_glass, material_ground} },
        ModelComponent{ model_car },
        PositionProxy16{
            -1, 0,  0, 0.5,     // X basis = (-1, 0, 0)
             0, 1,  0, 0.0,
             0, 0, -1, 0.0,     // Z basis = (0, 0, -1)
             0, 0,  0, 1.0
        }, ShadowComponent{}
    );
    ctx->CreateEntity("main_menu",
        MaterialComponent{ {material_car, material_car2, material_glass, material_ground} },
        ModelComponent{ model_car },
        PositionProxy16{
            -1, 0,  0, 0.5f,     // X basis = (-1, 0, 0)
             0, 1,  0, 0.0f,
             0, 0, -1, 2.5f,     // Z basis = (0, 0, -1)
             0, 0,  0, 1.0f
        }, ShadowComponent{}
    );
    Entity parent_id = ctx->CreateEntity("main_menu",
        MaterialComponent{ {material_car, material_car2, material_glass, material_ground} },
        ModelComponent{ model_car },
        PositionProxy16{ 1,0,0,3,  0,1,0,0,  0,0,1,2.5f,  0,0,0,1 },
        ShadowComponent{}
    );
    //ctx->CreateEntity("main_menu",
    //    SpotLightComponent{ SpotLightComponent::SpotLightData{ 0, 1.0f, 0.0f, 0.0f, 0.18f, 1, 1, 1, 100 } },
    //    PositionProxy16{ 1,0,0,-2.5f,  0,1,0,0,  0,0, 1,1.25f,  0,0,0,1 },
    //    ShadowCasterComponent{}
    //);
    ctx->CreateEntity("main_menu",
        SphereLightComponent{ SphereLightComponent::SphereLightData{ 0.0125f, 1.0f, 1.0f, 1.0f, 5.0f, 20.0f } },
        PositionProxy16{ 1,0,0, 0.0f,  0,1,0,0,  0,0, 1,1.25f,  0,0,0,1 },
        ShadowCasterComponent{}
    );


    ChangeState(GameState::MAIN_MENU);
    
    return SDL_APP_CONTINUE;
}

SDL_AppResult Game::MainIterate()
{
    input->DrainKeyEvents(key_events_scratch);
    for (const InputManager::KeyEvent& e : key_events_scratch) {
        if (!e.down) continue;
        switch (e.scancode) {
        case SDL_SCANCODE_ESCAPE:
            // Выход / меню
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
    camera->RotateView(mouse_x, mouse_y, input->IsMouseButtonDown(SDL_BUTTON_LEFT));

    this->MainMenu_Update();

    switch (game_state) {
    case GameState::MAIN_MENU:
        MainMenu_Iterate();
        break;
    }

    input->ExecuteCommands(ctx);

    return SDL_APP_CONTINUE;
}

SDL_AppResult Game::SDL_AppEvent(SDL_Event* event)
{
    switch (event->type)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_RESIZED:
        engine->OnWindowResized(event->window.data1, event->window.data2);
        return SDL_APP_CONTINUE;
    default:
        // Игровой ввод больше не обрабатывается на main-потоке: он уходит
        // в InputManager и дренится в sim-потоке (см. MainIterate).
        return SDL_APP_CONTINUE;
    }
}

void Game::SDL_AppQuit()
{
    ChangeState(GameState::MAIN_MENU);
    MainMenu_Quit();
}

void Game::ChangeState(GameState newState)
{
    bool skipQuit = ((game_state == GameState::MAIN_MENU && newState == GameState::SETTINGS) or (newState == GameState::MAIN_MENU && game_state == GameState::SETTINGS));
    bool skipInit = skipQuit;

    if (!skipQuit && newState != game_state) {
        switch (game_state) {
        case GameState::MAIN_MENU:
            MainMenu_Quit();
            break;
        default:
            break;
        }
    }

    game_state = newState;

    if (!skipInit) {
        switch (game_state) {
        case GameState::MAIN_MENU:
            MainMenu_Init();
            break;
        default:
            break;
        }
    }
}

void Game::MainMenu_Init()
{
}

void Game::MainMenu_Iterate()
{
}

void Game::MainMenu_Update()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;

    const float camSpeed = 0.05f;
    const float lightSpeed = 0.1f;

    glm::vec3 camMove(0.0f);     // x: лево/право, y: верх/низ, z: вперёд/назад
    glm::vec3 lightMove(0.0f);   // x: P.w, y: P.h, z: P.d
    bool camMoved = false;
    bool lightMoved = false;

    // Перебираем зажатые клавиши switch'ем, накапливая дельты.
    input->SnapshotHeldKeys(held_keys_scratch);
    for (SDL_Scancode sc : held_keys_scratch) {
        switch (sc) {
        case SDL_SCANCODE_LEFT:   camMove.x -= camSpeed; camMoved = true; break;
        case SDL_SCANCODE_RIGHT:  camMove.x += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_UP:     camMove.z += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_DOWN:   camMove.z -= camSpeed; camMoved = true; break;
        case SDL_SCANCODE_SPACE:  camMove.y += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_LSHIFT: camMove.y -= camSpeed; camMoved = true; break;

        case SDL_SCANCODE_A: lightMove.x -= lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_D: lightMove.x += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_W: lightMove.y += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_S: lightMove.y -= lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_E: lightMove.z += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_Q: lightMove.z -= lightSpeed; lightMoved = true; break;
        default: break;
        }
    }

    // Применяем накопленное по одному разу.
    if (camMoved) cameraManager->GetActiveCamera()->Move(camMove);

    if (lightMoved) {
        SceneData* scene = objectManager->GetActiveScene();
        objectManager->ForEach<Positions, SpotLightComponent>(scene,
            [lightMove](SoAElement<Positions> pos_el, SpotLightComponent&)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            P.w[i] += lightMove.x; P.h[i] += lightMove.y; P.d[i] += lightMove.z;
        });
        objectManager->ForEach<Positions, SphereLightComponent>(scene,
            [lightMove](SoAElement<Positions> pos_el, SphereLightComponent&)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            P.w[i] += lightMove.x; P.h[i] += lightMove.y; P.d[i] += lightMove.z;
        });
    }
}

void Game::MainMenu_Event(SDL_Event* event)
{
}


void Game::MainMenu_Quit()
{
    // Пока ничего
}
