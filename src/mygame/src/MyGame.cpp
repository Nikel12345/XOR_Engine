#include "PCH.h"
#include "MyGame.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "EngineContext.h"
#include "ObjectManager.h"
#include "CameraManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "InputManager.h"
#include "ThreadController.h"
#include "LightDataModule.h"
#include "imgui.h"
#include "DefaultShaderSet.h"
#include "FractalBackground.h"   // кадр фрактала (буфер+апдейтер) + FractalScaleStep (I/K)

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
}

SDL_AppResult MyGame::MainInit()
{
    Camera* camera = cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);
    camera->SetView(
        glm::vec3(2.2f, 1.4f, 3.2f), // позиция камеры
        // Взгляд на центр губки Менгера (ноль мира, полуразмер 1); вращение — ЛКМ,
        // приближение к поверхности раскрывает уровни детализации адаптивно.
        glm::vec3(-0.533f, -0.339f, -0.775f),
        glm::vec3(0.0f, 1.0f, 0.0f)  // вектор вверх
    );

    {
        // Только COMPUTE-программы: они держат указатели на буферы/атласы + dispatch_func, не
        // сериализуются. CSD (culling/bloom) приезжают из saved_scene_fractal/shaders.json;
        // csp хранит имя cs — резолв на сборке compute-пайплайна (после LoadScene).
        using namespace DefaultShaderProgramSet;
        SetBloomPrograms(ctx);
        SetCullingPibPrograms(ctx, engine->GetLightDataModule());   // GPU-каллинг: out_pib
    }

    // Кадр фрактала (буфер "_FractalFrameBuffer" + апдейтер-ребейз) — ДО LoadScene: sp "Fractal"
    // из манифеста сцены ссылается на буфер по имени, резолв идёт по уже существующим.
    FractalBackground::CreateFractalFrameResources(ctx);

    // Пере-привязку push-констант к sp движок зовёт сам В КОНЦЕ каждой загрузки (в т.ч.
    // UI-рантаймовой): sp из манифеста пересозданы голыми. Регистрируем ДО первого LoadScene.
    engine->SetBindShaderFunctions([this] { DefaultShaderProgramSet::BindDefaultPushFuncs(ctx); });

    objectManager->CreateScene("scene_fractal");
    ctx->LoadScene("scene_fractal", "saved_scene_fractal");   // папка сцены (scene.json + ресурсы)

    return SDL_APP_CONTINUE;
}

SDL_AppResult MyGame::MainIterate()
{
    // Клавишных «событий» у фрактала больше нет: масштаб — непрерывный, на удержании
    // I/K в блоке полёта ниже. Очередь событий всё равно дренируем (транспорт ввода).
    input->DrainKeyEvents(key_events_scratch);

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

    // Полёт: стрелки — горизонталь/вперёд, Space/LShift — вертикаль. Камера — аккумулятор
    // сдвига в ПОСТОЯННЫХ игровых юнитах: мировой масштаб (АВТО: следует за расстоянием до
    // поверхности) применяет FractalBackground при внесении дельты. «Зум» = лететь к грани:
    // подлёт сам замедляется и раскрывает детализацию, врезаться нельзя.
    // Колесо (SpeedChange камеры) — множитель скорости поверх; удержание I/K — ручной сдвиг
    // окна масштаба относительно авто (~1 уровень в секунду, I — мельче, K — крупнее).
    if (!io.WantCaptureMouse) {
        const float camSpeed = 1.0f;           // игровые юниты за тик (полное отклонение)
        const float zoomRate = 1.0f / 60.0f;   // уровней за тик удержания (при 60 UPS = 1 ур/с)
        glm::vec3 camMove(0.0f);   // x: лево/право, y: верх/низ, z: вперёд/назад
        bool camMoved = false;

        input->SnapshotHeldKeys(held_keys_scratch);
        for (SDL_Scancode sc : held_keys_scratch) {
            switch (sc) {
            case SDL_SCANCODE_LEFT:   camMove.x -= camSpeed; camMoved = true; break;
            case SDL_SCANCODE_RIGHT:  camMove.x += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_UP:     camMove.z += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_DOWN:   camMove.z -= camSpeed; camMoved = true; break;
            case SDL_SCANCODE_SPACE:  camMove.y += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_LSHIFT: camMove.y -= camSpeed; camMoved = true; break;

            case SDL_SCANCODE_I: FractalBackground::FractalScaleStep(+zoomRate); break;  // глубже
            case SDL_SCANCODE_K: FractalBackground::FractalScaleStep(-zoomRate); break;  // наружу
            default: break;
            }
        }
        if (camMoved) camera->Move(camMove);
    }

    return SDL_APP_CONTINUE;
}
