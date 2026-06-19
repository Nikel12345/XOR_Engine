#include "PCH.h"
#include "MyGame.h"
#include "imgui.h"

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
    // Первая созданная сцена становится активной (SceneData::is_active == true по умолчанию).
    objectManager->CreateScene("main");

    Camera* camera = cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);
    camera->SetView(
        glm::vec3(2.0f, 0.7f, 3.5f),    // позиция камеры
        glm::vec3(0.43f, -0.4f, -0.8f), // точка взгляда
        glm::vec3(0.0f, 1.0f, 0.0f)     // вектор вверх
    );

    // TODO: здесь поднимаешь свои ресурсы и сущности через ctx:
    //   ctx->CreateTextureAtlas(...);
    //   ctx->CreateTextureFromFile(...);
    //   ctx->CreateMaterial(...);
    //   ctx->CreateModel(...);
    //   ctx->CreateEntity("main", ...);
    // Шейдерные программы/проходы — свой ShaderSet (по образцу DefaultShaderSet в game).

    return SDL_APP_CONTINUE;
}

SDL_AppResult MyGame::MainIterate()
{
    // Дренаж разовых нажатий.
    input->DrainKeyEvents(key_events_scratch);
    for (const InputManager::KeyEvent& e : key_events_scratch) {
        if (!e.down) continue;
        switch (e.scancode) {
        case SDL_SCANCODE_ESCAPE:
            // выход / меню
            break;
        default:
            break;
        }
    }

    // Камера: колесо — скорость, ЛКМ — вращение.
    Camera* camera = cameraManager->GetActiveCamera();
    ImGuiIO& io = ImGui::GetIO();

    float wheel = input->ConsumeWheelDelta();
    if (wheel != 0.0f && !io.WantCaptureMouse) {
        camera->SpeedChange(wheel * 0.5f);
    }
    mouse_x = input->MouseX();
    mouse_y = input->MouseY();
    camera->RotateView(mouse_x, mouse_y, input->IsMouseButtonDown(SDL_BUTTON_LEFT));

    // Отложенные команды ввода применяются к ECS здесь.
    input->ExecuteCommands(ctx);

    return SDL_APP_CONTINUE;
}

SDL_AppResult MyGame::SDL_AppEvent(SDL_Event* event)
{
    switch (event->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_RESIZED:
        engine->OnWindowResized(event->window.data1, event->window.data2);
        return SDL_APP_CONTINUE;
    default:
        // Игровой ввод обрабатывается не здесь: он уходит в InputManager
        // и дренится в sim-потоке (см. MainIterate).
        return SDL_APP_CONTINUE;
    }
}
