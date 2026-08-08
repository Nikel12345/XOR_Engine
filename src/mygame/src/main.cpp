#include "PCH.h"
#include "Engine.h"
#include "MyGame.h"
#include "ThreadController.h"
#include "InputManager.h"
#include "config.h"
#include "UI_ImGui.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;
static SDL_Window* win = NULL;
static SDL_GPUDevice* dev = NULL;
static constexpr float WIDTH = 800.0f;
static constexpr float HEIGHT = 600.0f;

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    auto make_window = [] {
        return SDL_CreateWindow("MyGame",
            static_cast<int>(WIDTH), static_cast<int>(HEIGHT), SDL_WINDOW_RESIZABLE);
    };
    win = make_window();
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    // ТОЛЬКО SPIRV: движок компилирует шейдеры единственным путём и отдаёт compute-пайплайны
    // сырым SPIR-V (PipeManager::GetOrCreateComputePipeline). Перечислить тут DXIL/MSL — значит
    // разрешить SDL выбрать бэкенд, для которого у нас нет байткода: на SDL 3.4 авто-выбор на
    // Windows уходит в D3D12, и compute-пайплайны падают на «not valid DXIL».
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!dev) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Log("GPU backend: %s", SDL_GetGPUDeviceDriver(dev));
    SDL_ClaimWindowForGPUDevice(dev, win);

    // БАГ SDL 3.4.14: первое окно процесса Vulkan-девайс не заклеймливает (claim возвращает true,
    // но окно не регистрируется). Подробности и разбор — в game/src/main.cpp и в зонде
    // sandbox/src/ClaimWindowProbe.cpp. Проверяем факт, а не возврат; ветка сама отомрёт с фиксом.
    if (SDL_GetGPUSwapchainTextureFormat(dev, win) == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("Claim didn't take (SDL 3.4 first-window bug) - recreating window");
        SDL_ReleaseWindowFromGPUDevice(dev, win);
        SDL_DestroyWindow(win);
        win = make_window();
        if (!win || !SDL_ClaimWindowForGPUDevice(dev, win)) {
            SDL_Log("Window re-claim failed: %s", SDL_GetError());
            return 1;
        }
    }
    SDL_SetGPUAllowedFramesInFlight(dev, BUFFERING_LEVEL);

    SDL_GPUPresentMode desired_mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_GPUSwapchainComposition desired_comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

    if (!SDL_WindowSupportsGPUPresentMode(dev, win, desired_mode)) {
        SDL_Log("IMMEDIATE mode not supported - falling back to VSYNC");
        desired_mode = SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (!SDL_WindowSupportsGPUSwapchainComposition(dev, win, desired_comp)) {
        SDL_Log("SDR composition not supported - fallback to default");
        desired_comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    }
    if (!SDL_SetGPUSwapchainParameters(dev, win, desired_comp, desired_mode)) {
        SDL_Log("Failed to set swapchain parameters: %s", SDL_GetError());
    }
    else {
        SDL_Log("Swapchain set: comp=%d, mode=%d", desired_comp, desired_mode);
    }

    Engine* engine = new Engine(win, dev, WIDTH, HEIGHT);
    MyGame* game = new MyGame(engine);

    game->MainInit();

    ThreadController* threadController = engine->GetThreadController();
    if (!threadController) {
        SDL_Log("Failed to get ThreadController");
        return -1;
    }

    threadController->SetGameIterationCallback([game] {
        game->MainIterate();
    });

    InputManager* input = engine->GetInputManager();

    threadController->StartThreads();
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            UI_ImGui::ProcessEvent(event);

            // События окна/жизненного цикла — здесь, где есть engine и running.
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { running = false; break; }
            if (event.type == SDL_EVENT_WINDOW_RESIZED)
                // window-пара из события; render-пара (0,0) пока не используется — внутреннее
                // разрешение зафиксировано в движке, картинка тянется на окно present-блитом.
                engine->OnWindowResized(event.window.data1, event.window.data2, 0, 0);

            // Игровой ввод — в транспорт, дренит sim-поток.
            input->HandleEvent(event);
        }
        SDL_Delay(16);
    }
    return 0;
}
