#include "PCH.h"
#include <cmath>
#include "Game.h"
#include "TexturesPresets.h"
#include "DefaultShaderSet.h"
#include "MaterialParams.h"        // SetMaterialParams + раскладки факторов материалов
#include "Colliders.h"             // из либы Physics: компоненты коллайдеров
#include "ContactSystem.h"         // детекция контактов
#include "DebugColliderSystem.h"   // отладочные рамки

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

    TextureAtlas* atlas = ctx->CreateTextureAtlas("albedo_atlas", TexturePresets::AlbedoAtlas(2048, 3, TexturePresets::FullMipLevels(2048)), DefaultSamplersNames::DEFAULT_SAMPLER);
	// Мипы обязательны: без них (а) POM-префильтр pomBias — no-op (нет грубых уровней), (б) нормаль
	// не сглаживается (мерцание, см. заметку про red-shift). FullMipLevels включает мип-цепочку.
	TextureAtlas* normal_atlas   = ctx->CreateTextureAtlas("normal_atlas",   TexturePresets::NormalAtlas(2048, 2, TexturePresets::FullMipLevels(2048)),   DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* orm_atlas      = ctx->CreateTextureAtlas("orm_atlas",      TexturePresets::ORMAtlas(2048, 2), DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* emissive_atlas = ctx->CreateTextureAtlas("emissive_atlas", TexturePresets::EmissiveAtlas(1024, 1), DefaultSamplersNames::DEFAULT_SAMPLER);

	TextureHandle* texture_cube = ctx->CreateTextureFromFile("albedo_cube", "albedo_atlas", "textures/assets/cube_test.png");
	TextureHandle* norm = ctx->CreateTextureFromFile("norm", "normal_atlas", "textures/assets/car_norm.png");

    ctx->CreateTextureFromFile("texture_asphalt", "albedo_atlas", "textures/blocks/brick_base_c.png");
    // G у этой ORM хранит smoothness (asphalt G≈0.12 → как roughness это «мокрое зеркало»).
    // Нормализуем к канону движка (roughness) инверсией G на импорте.
    ctx->CreateTextureFromFile("texture_asphalt_orm", "orm_atlas", "textures/blocks/brick_orm.png", ChannelConvention::SmoothnessInGreen);
    // Normal с картой ВЫСОТ в альфе (RGB=нормаль, A=height: яркое=выше; POM читает depth=1-A из
    // u_normal.a — без нового атласа/слота). AsIs: и нормаль в конвенции движка, и альфа height-стиля
    // (у бороздчатой коры средняя яркость низкая — это НЕ признак cavity-карты; проверено глазами).
    // DepthInAlpha — только для действительно перевёрнутых (cavity) альф. Для волокнистых карт
    // укрупняй рельеф pomBias ~2..3 (абсолютный пол LOD; НЕ выключатель POM — выключатель heightScale=0).
    ctx->CreateTextureFromFile("wood_norm", "normal_atlas", "textures/blocks/brick_normal_h.png", ChannelConvention::AsIs);

	textureManager->CreateTexture("default_orm",      "orm_atlas",      2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));
	textureManager->CreateTexture("default_emissive", "emissive_atlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));

    TextureHandle* texture_car = ctx->CreateTextureFromFile("new_car", "albedo_atlas", "textures/assets/new_car.png");
	TextureHandle* ground = ctx->CreateTextureFromFile("new_car_ground", "albedo_atlas", "textures/assets/new_car_ground.png");
	TextureHandle* glass = ctx->CreateTextureFromFile("new_car_glass", "albedo_atlas", "textures/assets/Tex_Glass.jpg");

    {
        using namespace DefaultShaderProgramSet;
        SetMainShaderProgram(ctx);
        SetDefaultShadowShaderProgram(ctx);
        SetTransparentShaderProgram(ctx);
        SetUntexturedShaderProgram(ctx);
        SetDebugColliderProgram(ctx);
        SetBloomPrograms(ctx); 
    }
    
    auto material_car = ctx->CreateMaterial("car", {
        {TextureSlotRole::Albedo, "new_car"},
        {TextureSlotRole::Normal, "norm"},
        {TextureSlotRole::ORM, "default_orm"},
        {TextureSlotRole::Emissive, "default_emissive"} },
        { "sp", "sp_shadow" });
	auto material_car2 = ctx->CreateMaterial("car2", {
        {TextureSlotRole::Albedo, "new_car"},
        {TextureSlotRole::Normal, "norm"},
        {TextureSlotRole::ORM, "default_orm"},
        {TextureSlotRole::Emissive, "default_emissive"} },
		{ "sp", "sp_shadow" });
    auto material_ground = ctx->CreateMaterial("ground", {
        {TextureSlotRole::Albedo, "new_car_ground"},
        {TextureSlotRole::Normal, "norm"},
        {TextureSlotRole::ORM, "default_orm"},
        {TextureSlotRole::Emissive, "default_emissive"} },
		{ "sp", "sp_shadow" });

    auto material_sprite = ctx->CreateMaterial("material_sprite", {
        {TextureSlotRole::Albedo, "texture_asphalt"},
        {TextureSlotRole::Normal, "wood_norm"},
        {TextureSlotRole::ORM, "texture_asphalt_orm"},
        {TextureSlotRole::Emissive, "default_emissive"} },
        { "sp", "sp_shadow" });
    // heightScale = глубина POM (0 = ВЫКЛючатель). pomBias = АБСОЛЮТНЫЙ пол LOD рельефа (лог2:
    // 2 = среднее по 4×4 соседям, 3 = 8×8): глушит высокочастотный шум карты («шпили») на любой
    // дистанции; 0 = полная детализация (кирпич с чётким швом). Для волокнистой коры — 2..3.
    ctx->SetMaterialParams(material_sprite, OpaqueMaterialParams{ {0.5f,0.5f,0.5f,1}, {0,0,0}, 1.0f, /*metallic*/1.0f, /*roughness*/1.0f, /*heightScale*/0.08f, /*pomBias*/2.5f });

    auto material_sprite2 = ctx->CreateMaterial("material_sprite2", {
        {TextureSlotRole::Albedo, "texture_asphalt"},
        {TextureSlotRole::Normal, "norm"},
        {TextureSlotRole::ORM, "default_orm"},
        {TextureSlotRole::Emissive, "default_emissive"} },
        { "sp", "sp_shadow" });
    ctx->SetMaterialParams(material_sprite2, OpaqueMaterialParams{ {1,1,1,1}, {0,0,0}, 1.0f, /*metallic*/1.0f, /*roughness*/1.0f });

    auto material_glass = ctx->CreateMaterial("transparent", {
        {TextureSlotRole::Albedo, "new_car_glass"},
        {TextureSlotRole::Normal, "norm"} },
        { "sp_transparent" });


    auto ship_material = ctx->CreateMaterial("ship", {}, { "sp_untextured" });

    auto m_orange = ctx->CreateMaterial("m_orange", {}, { "sp_untextured", "sp_shadow" });
    ctx->SetMaterialParams(m_orange, OpaqueMaterialParams{ {1.0f, 0.45f, 0.1f, 1.0f} });

    auto m_gray = ctx->CreateMaterial("m_gray", {}, { "sp_untextured", "sp_shadow" });
    ctx->SetMaterialParams(m_gray, OpaqueMaterialParams{ {0.5f, 0.5f, 0.5f, 1.0f} });

    auto metal1 = ctx->CreateMaterial("metal1", {}, { "sp_untextured", "sp_shadow" });
    ctx->SetMaterialParams(metal1, OpaqueMaterialParams{ {1.0f, 0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.96f });

    auto metal2 = ctx->CreateMaterial("metal2", {}, { "sp_untextured", "sp_shadow" });
    ctx->SetMaterialParams(metal2, OpaqueMaterialParams{ {0.5f, 0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.96f });

    auto emission = ctx->CreateMaterial("emission", {}, { "sp_untextured", "sp_shadow" });
    ctx->SetMaterialParams(emission, OpaqueMaterialParams{ {0.0f, 0.0f, 0.0f, 1.0f}, {0.3f, 0.3f, 0.6f}, 1.0f });

    ctx->SetMaterialParams(ship_material, OpaqueMaterialParams{ { 0.55f, 0.6f, 0.7f, 1.0f } });

    ctx->SetMaterialParams(material_glass, TransparentMaterialParams{ 0.35f });

    // Opaque-материалы тоже несут params (дефолт-белый baseColorFactor = без тинта): иначе их
    // MaterialBlock @ b1 остался бы несвязанным. metallic/roughness/emission — задел (закомм.).
    ctx->SetMaterialParams(material_car,    OpaqueMaterialParams{});
    ctx->SetMaterialParams(material_car2,   OpaqueMaterialParams{});
    ctx->SetMaterialParams(material_ground, OpaqueMaterialParams{});

    debug_collider_material = ctx->CreateMaterial("debug_collider", {}, { "sp_debug_collider" });

    ModelData* model_car = ctx->CreateModel("car", "models/new_car_n_fixed.bin", "models/new_car_n_fixed_i.bin");
	ModelData* model_ship = ctx->CreateModel("ship", "models/low_poly_ship.bin", "models/low_poly_ship_i.bin");

    // ВАЖНО: развёртка обязана быть ПРАВОсторонней (du × dv = +N), потому что вершинник строит
    // битангент как cross(N, T) без tangent.w. Старая развёртка (v = 1 - y) была левосторонней →
    // ось v tangent-пространства инвертировалась: POM марчил по Y в обратную сторону (искажения,
    // «работает по Y, слабо по X»), а свет по v требовал компенсации G-флипом. v = y — согласовано
    // со сферой; текстура на кваде отобразится вертикально зеркально (для кирпича неважно).
    ModelData* quad = ctx->CreateModel("quad", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
        v = {
            { 0,0,0,  0,0,  0,0,1,  1,0,0 },
            { 1,0,0,  1,0,  0,0,1,  1,0,0 },
            { 1,1,0,  1,1,  0,0,1,  1,0,0 },
            { 0,1,0,  0,1,  0,0,1,  1,0,0 },
        };
        i = { 0, 1, 2, 0, 2, 3 };
    });

    ModelData* sphere = ctx->CreateModel("sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
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
    });

    debug_box_model = ctx->CreateModel("debug_box", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
        auto P = [](float x, float y, float z) {
            PosUVNormal vert{}; vert.x = x; vert.y = y; vert.z = z; return vert;
        };
        v = {
            P(-1,-1,-1), P(1,-1,-1), P(1, 1,-1), P(-1, 1,-1),   // 0..3  z=-1
            P(-1,-1, 1), P(1,-1, 1), P(1, 1, 1), P(-1, 1, 1),   // 4..7  z=+1
        };
        i = {
            0,1, 1,2, 2,3, 3,0,   // рёбра грани z=-1
            4,5, 5,6, 6,7, 7,4,   // рёбра грани z=+1
            0,4, 1,5, 2,6, 3,7,   // рёбра вдоль z
        };
    });

    debug_sphere_model = ctx->CreateModel("debug_sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
        const uint32_t stacks = 8, slices = 12;
        const float PI = 3.14159265358979323846f;
        for (uint32_t i = 0; i <= stacks; ++i) {
            float phi = PI * (float)i / (float)stacks;
            float cp = std::cos(phi), sp = std::sin(phi);
            for (uint32_t j = 0; j <= slices; ++j) {
                float theta = 2.0f * PI * (float)j / (float)slices;
                PosUVNormal vert{};
                vert.x = sp * std::cos(theta); vert.y = cp; vert.z = sp * std::sin(theta);
                v.push_back(vert);
            }
        }
        const uint32_t row = slices + 1;
        // параллели: ребро a -> a+1 вдоль каждого ряда
        for (uint32_t i = 0; i <= stacks; ++i)
            for (uint32_t j = 0; j < slices; ++j) {
                uint32_t a = i * row + j;
                idx.push_back(a); idx.push_back(a + 1);
            }
        // меридианы: ребро a -> a+row между рядами
        for (uint32_t i = 0; i < stacks; ++i)
            for (uint32_t j = 0; j <= slices; ++j) {
                uint32_t a = i * row + j;
                idx.push_back(a); idx.push_back(a + row);
            }
    });


    //ctx->CreateEntity("main_menu",
    //    SpotLightComponent{ SpotLightComponent::SpotLightData{ 0, 1.0f, 0.0f, 0.0f, 0.18f, 1,\sd   1, 1, 100 } },
    //    PositionProxy16{ 1,0,0,-2.5f,  0,1,0,0,  0,0, 1,1.25f,  0,0,0,1 },
    //    ShadowCasterComponent{}
    //);
    //ctx->CreateEntity("main_menu",
    //    SphereLightComponent{ SphereLightComponent::SphereLightData{ 0.0125f, 1.0f, 1.0f, 1.0f, 5.0f, 20.0f } },
    //    PositionProxy16{ 1,0,0, 0.0f,  0,1,0,0,  0,0, 1,1.25f,  0,0,0,1 },
    //    ShadowCasterComponent{},
    //    ColliderComponent{}
    //);


    ctx->RegisterGenerator("main_menu", [this] { CreateDebugColliders(); });
    ctx->LoadScene("main_menu", "saved_scene.scene");

    ctx->CreateEntity("main_menu",
        MaterialComponent{ { metal2 } },
        ModelComponent{ sphere },
        PositionProxy16{ 1,0,0,-2.0f,  0,1,0,0.7f,  0,0,1,0,  0,0,0,1 },
        ShadowComponent{},
        ColliderComponent{ { Collider::Sphere(1.0f)} },
        DrawComponent{}
    );

    ctx->ExecuteGenerators();


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
    bool rotate = input->IsMouseButtonDown(SDL_BUTTON_LEFT) && !io.WantCaptureMouse;
    camera->RotateView(mouse_x, mouse_y, rotate);

    input->ExecuteCommands(ctx);

    this->MainMenu_Update();

    switch (game_state) {
    case GameState::MAIN_MENU:
        MainMenu_Iterate();
        break;
    }

    // Энтити с ColliderComponent берут явный радиус; остальные с ModelComponent — модельную сферу.
    if (SceneData* scene = objectManager->GetActiveScene()) {
        for (const ContactSystem::Contact& c : ContactSystem::DetectContacts(*objectManager, scene)) {
            //SDL_Log("contact %u <-> %u (pen %.3f)", c.a, c.b, c.penetration);
        }
    }

    return SDL_APP_CONTINUE;
}

void Game::CreateDebugColliders()
{
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;
    if (!debug_collider_material || !debug_box_model || !debug_sphere_model) return;

    std::vector<DebugColliderSystem::DebugShape> shapes = DebugColliderSystem::CollectDebugShapes(*objectManager, scene);
    if (shapes.empty()) return;

    for (const DebugColliderSystem::DebugShape& s : shapes) {
        ModelData* model = (s.kind == ShapeKind::Box) ? debug_box_model : debug_sphere_model;
        LocalMatrixProxy16 lm{};   // SoA-локаль: в CreateEntity едет как прокси (как PositionProxy16)
        for (int i = 0; i < 16; ++i) lm.m[i] = s.local[i];
        ctx->CreateEntity("main_menu",
            MaterialComponent{ { debug_collider_material } },
            ModelComponent{ model },
            PositionProxy16{},          // перезапишется композицией parent × local
            ParentComponent{ s.owner },
            lm,
            DrawComponent{ false, 1.0f, 0 },
            DebugColliderTag{},
            EditorHiddenComponent{},    // движковый тег: не показывать в списке объектов UI
            GeneratedComponent{});      // сгенерировано кодом → не сериализуется, пересоздаётся генератором
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
