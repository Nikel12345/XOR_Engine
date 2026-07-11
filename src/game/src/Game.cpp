#include "PCH.h"
#include <cmath>
#include "Game.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "EngineContext.h"
#include "ObjectManager.h"
#include "CameraManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"   // GetMaterial — переприменение params к загруженным материалам
#include "InputManager.h"
#include "ThreadController.h"
#include "LightDataModule.h"
#include "imgui.h"              // раньше транзитивно через Engine.h → UI_ImGui.h
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

	// Текстуры (хендлы) грузятся из сцены (textures.json) — атласы выше остаются кодовыми (инфра,
	// не сериализуются; текстуры садятся в них при загрузке). default_* — движковые (InitDefaultResources).

    {
        using namespace DefaultShaderProgramSet;
        SetBloomPrograms(ctx);
        SetCullingPibPrograms(ctx, engine->GetLightDataModule());   // GPU-каллинг: out_pib
    }

    ModelData* sphere = (*ctx->GetModelManager())["sphere"];

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

    // --- Процедурные параллелепипеды "cube_0".."cube_(N-1)" для сцены из scene_gen.py ---
    // ВАЖНО: kCubeVariants должен совпадать с NUM_CUBE_MODELS в scripts/scene_gen.py —
    // питон-скрипт раздаёт сущностям имена именно из этого диапазона.
    const int kCubeVariants = 12;
    for (int ci = 0; ci < kCubeVariants; ++ci) {
        // Разные пропорции коробки из индекса (детерминированно, полу-размеры 0.3..1.1).
        const float hx = 0.3f + 0.2f * float((ci * 7)  % 5);
        const float hy = 0.3f + 0.2f * float((ci * 3)  % 5);
        const float hz = 0.3f + 0.2f * float((ci * 11) % 5);
        ctx->CreateModel("cube_" + std::to_string(ci),
            [hx, hy, hz](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
            const float H[3] = { hx, hy, hz };
            // 6 граней. c — угол-начало, U/V — рёбра (в долях полу-размеров, per-axis);
            // cross(U,V) = ВНЕШНЯЯ нормаль → CCW наружу (как у quad). p0=c, p1=c+U, p2=c+U+V, p3=c+V.
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
                // Касательная = нормализованное направление U в мировых пропорциях.
                float tx = fd.U[0]*H[0], ty = fd.U[1]*H[1], tz = fd.U[2]*H[2];
                float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
                if (tl > 0.0f) { tx /= tl; ty /= tl; tz /= tl; }
                const uint32_t vbase = static_cast<uint32_t>(v.size());
                for (int q = 0; q < 4; ++q) {
                    PosUVNormal vert{};
                    vert.x = (fd.c[0] + uv[q][0]*fd.U[0] + uv[q][1]*fd.V[0]) * H[0];
                    vert.y = (fd.c[1] + uv[q][0]*fd.U[1] + uv[q][1]*fd.V[1]) * H[1];
                    vert.z = (fd.c[2] + uv[q][0]*fd.U[2] + uv[q][1]*fd.V[2]) * H[2];
                    vert.u = uv[q][0]; vert.v = uv[q][1];
                    vert.nx = fd.N[0]; vert.ny = fd.N[1]; vert.nz = fd.N[2];
                    vert.tx = tx;      vert.ty = ty;      vert.tz = tz;
                    v.push_back(vert);
                }
                idx.push_back(vbase + 0); idx.push_back(vbase + 1); idx.push_back(vbase + 2);
                idx.push_back(vbase + 0); idx.push_back(vbase + 2); idx.push_back(vbase + 3);
            }
        }, AnchorShift::Keep, /*dont_save=*/true);   // процедурные кубы игры — в models.json не идут
    }


    //ctx->CreateEntity("main_menu",
    //    SpotLightComponent{ SpotLightComponent::SpotLightData{ 0, 1.0f, 0.0f, 0.0f, 0.18f, 1,\sd   1, 1, 100 } },
    //    PositionProxy16{ 1,0,0,-2.5f,  0,1,0,0,  0,0, 1,1.25f,  0,0,0,1 },
    //    ShadowCasterComponent{}
    //);



    // Пере-привязку push-констант к sp движок зовёт сам В КОНЦЕ каждой загрузки (в т.ч.
    // UI-рантаймовой): sp из манифеста пересозданы голыми, а перенос по имени ломался бы
    // на переименовании. Регистрируем ДО первого LoadScene.
    engine->SetBindShaderFunctions([this] { DefaultShaderProgramSet::BindDefaultPushFuncs(ctx); });

    ctx->RegisterGenerator("main_menu", [this] { CreateDebugColliders(); });
    ctx->LoadScene("main_menu", "saved_scene");   // папка сцены (scene.json + ресурсы внутри)

    debug_collider_material = ctx->GetMaterialManager()->GetMaterial("debug_collider");

    // BindDefaultPushFuncs здесь больше не зовём: движок дёргает зарегистрированный колбэк
    // сам в конце LoadScene (см. SetBindShaderFunctions выше).

    {
        MaterialManager* mm = ctx->GetMaterialManager();
        ctx->SetMaterialParams(mm->GetMaterial("material_sprite"),  OpaqueMaterialParams{ {0.5f,0.5f,0.5f,1}, {0,0,0}, 1.0f, /*metallic*/1.0f, /*roughness*/1.0f, /*heightScale*/0.08f, /*pomBias*/2.5f });
        ctx->SetMaterialParams(mm->GetMaterial("material_sprite2"), OpaqueMaterialParams{ {1,1,1,1}, {0,0,0}, 1.0f, /*metallic*/1.0f, /*roughness*/1.0f });
        ctx->SetMaterialParams(mm->GetMaterial("transparent"),      TransparentMaterialParams{ 0.35f });
        ctx->SetMaterialParams(mm->GetMaterial("material_sun"),     OpaqueMaterialParams{ {1,1,1,1}, {0.99f,0.85f,0.45f}, 2.4f, /*metallic*/0.0f, /*roughness*/1.0f });
        ctx->SetMaterialParams(mm->GetMaterial("ship"),             OpaqueMaterialParams{ { 0.55f, 0.6f, 0.7f, 1.0f } });
        ctx->SetMaterialParams(mm->GetMaterial("m_orange"),         OpaqueMaterialParams{ {1.0f, 0.45f, 0.1f, 1.0f}, {1.0f, 0.854902f, 0.0f}, 0.171f });
        ctx->SetMaterialParams(mm->GetMaterial("m_gray"),           OpaqueMaterialParams{ {0.5f, 0.5f, 0.5f, 1.0f}, {0.552941f, 0.552941f, 0.552941f}, 1.2f });
        ctx->SetMaterialParams(mm->GetMaterial("metal1"),           OpaqueMaterialParams{ {1.0f, 0.5f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0.146f, 1.0f, 0.96f });
        ctx->SetMaterialParams(mm->GetMaterial("metal2"),           OpaqueMaterialParams{ {0.5f, 0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.96f });
        ctx->SetMaterialParams(mm->GetMaterial("emission"),         OpaqueMaterialParams{ {0.0f, 0.0f, 0.0f, 1.0f}, {0.341176f, 0.341176f, 0.647059f}, 1.7f });
        // Opaque-дефолт (белый baseColor = без тинта): иначе MaterialBlock @ b1 несвязан.
        ctx->SetMaterialParams(mm->GetMaterial("car"),              OpaqueMaterialParams{});
        ctx->SetMaterialParams(mm->GetMaterial("car2"),             OpaqueMaterialParams{});
        ctx->SetMaterialParams(mm->GetMaterial("ground"),           OpaqueMaterialParams{});
    }

    Material* metal2 = ctx->GetMaterialManager()->GetMaterial("metal2");   // из сцены (было кодовым)
    Entity sun = ctx->CreateEntity("main_menu",
        MaterialComponent{ { metal2 } },
        ModelComponent{ sphere },
        PositionProxy16{ 1,0,0,0.0f, 0,1,0,0.7f, 0,0,1,0, 0,0,0,1 },
        ShadowComponent{},
        ColliderComponent{ { Collider::Sphere(1.0f)} },
        DrawComponent{},
        GeneratedComponent{}
    );

    //ctx->CreateEntity("main_menu",
    //    ParentComponent{ sun },
    //    SphereLightComponent{ SphereLightComponent::SphereLightData{ 0.0125f, 1.0f, 1.0f, 1.0f, 5.0f, 20.0f } },
    //    LocalMatrixProxy16{},
    //    PositionProxy16{},
    //    ShadowCasterComponent{},
    //    ColliderComponent{}
    //);

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
        camera->SpeedChange(wheel);   // щелчки колеса; шаг мультипликативный (см. Camera::SPEED_STEP)
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
    //if (SceneData* scene = objectManager->GetActiveScene()) {
    //    for (const ContactSystem::Contact& c : ContactSystem::DetectContacts(*objectManager, scene)) {
    //        //SDL_Log("contact %u <-> %u (pen %.3f)", c.a, c.b, c.penetration);
    //    }
    //}

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
    SimulateGravity();   // симуляция идёт всегда, до раннего выхода по WantCaptureMouse

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

// Гравитация одного центрального объекта в (0,0) плоскости XZ + движение.
// kGravGM ДОЛЖЕН равняться GRAVITY_CONST * CENTRAL_MASS из scripts/scene_gen/scene_gen.py
// (сейчас 1 * 50 = 50): скорость круговой орбиты sqrt(GM/r) считает генератор — при другом GM
// орбиты станут эллиптичными/раскрутятся. Форму орбиты задаёт ТОЛЬКО GM.
// kSimDt — «скорость времени»: не влияет на форму орбиты, только на темп. У движка нет
// delta-time (камера/свет — тоже по кадрам), поэтому шаг фиксированный. При радиусах 50..350
// орбиты долгие — увеличивай kSimDt (или уменьшай радиусы диска в scene_gen.py), чтобы вращение
// было заметным; слишком большой шаг добавит прецессию/дрожание орбиты.
// Гравитация ПЛОСКАЯ: ускорение к центру считается по XZ-радиусу и меняет только vx/vz — так y
// (толщина диска) и vy остаются нулевыми/постоянными, диск не «распухает».
static constexpr float kGravGM   = 5000.0f;
static constexpr float kSimDt    = 0.05f;
static constexpr float kGravSoft = 1e-3f;   // защита от деления на ~0 у самого центра

void Game::SimulateGravity()
{
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;

    // Обходим только сущности с Transform И Velocity (кубы). Когда ВСЕ компоненты — SoA,
    // ForEach отдаёт целые массивы один раз на архетип (не поэлементно) — цикл по строкам сами.
    // Позиция — трансляция матрицы: w = x, d = y, h = z (row-major, индексы 3/7/11).
    objectManager->ForEach<Positions, Velocities>(scene,
        [](Positions& P, Velocities& V)
    {
        const size_t n = P.w.size();
        for (size_t i = 0; i < n; ++i) {
            const float x = P.w[i];   // трансляция X
            const float z = P.h[i];   // трансляция Z
            const float r2 = x * x + z * z;
            const float r  = std::sqrt(r2);

            // Полу-неявный Эйлер: сначала скорость (ускорение к центру a = GM/r^2, направление
            // -(x,z)/r), затем позиция — так орбита устойчивее.
            if (r > kGravSoft) {
                const float a = kGravGM / r2;
                V.x[i] -= (x / r) * a * kSimDt;
                V.z[i] -= (z / r) * a * kSimDt;
            }

            // Обновляем координаты позиции (wdh) скоростями (xyz).
            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

void Game::MainMenu_Event(SDL_Event* event)
{
}


void Game::MainMenu_Quit()
{
    // Пока ничего
}
