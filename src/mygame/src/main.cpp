#include "PCH.h"
#include "Engine.h"
#include "MyGame.h"
#include "ThreadController.h"   // Engine.h теперь forward-only
#include "InputManager.h"
#include "config.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;
static SDL_Window* win = NULL;
static SDL_GPUDevice* dev = NULL;
static constexpr float WIDTH = 800.0f;
static constexpr float HEIGHT = 600.0f;

int main() {
    win = SDL_CreateWindow("MyGame",
        static_cast<int>(WIDTH),
        static_cast<int>(HEIGHT),
        SDL_WINDOW_RESIZABLE);
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
        SDL_GPU_SHADERFORMAT_DXIL |
        SDL_GPU_SHADERFORMAT_MSL,
        true, nullptr);
    SDL_ClaimWindowForGPUDevice(dev, win);
    SDL_SetGPUAllowedFramesInFlight(dev, BUFFERING_LEVEL);

    SDL_GPUPresentMode desired_mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_GPUSwapchainComposition desired_comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

    if (!SDL_WindowSupportsGPUPresentMode(dev, win, desired_mode)) {
        SDL_Log("IMMEDIATE mode not supported — falling back to VSYNC");
        desired_mode = SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (!SDL_WindowSupportsGPUSwapchainComposition(dev, win, desired_comp)) {
        SDL_Log("SDR composition not supported — fallback to default");
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
            ImGui_ImplSDL3_ProcessEvent(&event);

            // События окна/жизненного цикла — здесь, где есть engine и running.
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { running = false; break; }
            if (event.type == SDL_EVENT_WINDOW_RESIZED)
                engine->OnWindowResized(event.window.data1, event.window.data2);

            // Игровой ввод — в транспорт, дренит sim-поток.
            input->HandleEvent(event);
        }
        SDL_Delay(16);
    }
    return 0;
}
