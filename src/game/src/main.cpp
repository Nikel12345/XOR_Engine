#include "PCH.h"
#include "Engine.h"
#include "Game.h"
#include "ThreadController.h"
#include "InputManager.h"
#include "config.h"
#include "UI_ImGui.h"
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
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
        return SDL_CreateWindow("GPU-triangle (basic)",
            static_cast<int>(WIDTH), static_cast<int>(HEIGHT), SDL_WINDOW_RESIZABLE);
    };
    win = make_window();
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    // ТОЛЬКО SPIRV: движок компилирует шейдеры единственным путём (LoadOrCompileSPIRV →
    // SDL_ShaderCross_CompileSPIRVFromHLSL), а compute-пайплайны отдаёт в SDL сырым SPIR-V
    // (PipeManager::GetOrCreateComputePipeline). Перечислять тут DXIL/MSL — значит разрешить
    // SDL выбрать бэкенд, для которого у нас нет байткода: на SDL 3.4 авто-выбор на Windows
    // уходит в D3D12, и все compute-пайплайны падают на «not valid DXIL». Запрос ровно того
    // формата, который мы умеем, — и есть контракт; SDL сам подберёт подходящий бэкенд.
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    // Отказ здесь раньше не проверялся, и поломка проявлялась каскадом «Must claim window
    // before…» из последующих запросов свопчейна — то есть симптомом, а не причиной.
    if (!dev) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Log("GPU backend: %s", SDL_GetGPUDeviceDriver(dev));
    SDL_ClaimWindowForGPUDevice(dev, win);

    // БАГ SDL 3.4.14: ПЕРВОЕ созданное в процессе окно Vulkan-девайс не заклеймливает —
    // ClaimWindowForGPUDevice возвращает true, но окно не регистрируется, и дальше весь свопчейн
    // отвечает «Must claim window before…». Второе окно клеймится штатно. Воспроизведено голым
    // SDL, без движка: sandbox/src/ClaimWindowProbe.cpp (там же отсеяны ложные версии — способ
    // выбора бэкенда, debug_mode, SDL_WINDOW_VULKAN, порядок создания девайсов: ни при чём).
    // Поэтому проверяем ФАКТ (формат свопчейна), а не возврат claim, и один раз пересоздаём окно.
    // Условная ветка: когда баг починят, она просто перестанет срабатывать.
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

    SDL_GPUPresentMode desired_mode = SDL_GPU_PRESENTMODE_MAILBOX;
    SDL_GPUSwapchainComposition desired_comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

    if (!SDL_WindowSupportsGPUPresentMode(dev, win, desired_mode)) {
        SDL_Log("MAILBOX mode not supported - falling back to VSYNC");
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
    Game* game = new Game(engine);

    game->MainInit();

	ThreadController* threadController = nullptr;
    threadController = engine->GetThreadController();
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
        while (SDL_PollEvent(&event))
        {
            UI_ImGui::ProcessEvent(event);

            // События окна/жизненного цикла — здесь, где есть engine и running.
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { running = false; break; }
            if (event.type == SDL_EVENT_WINDOW_RESIZED)
                // window-пара из события; render-пара (0,0) пока не используется — внутреннее
                // разрешение зафиксировано в движке, картинка тянется на окно present-блитом.
                engine->OnWindowResized(event.window.data1, event.window.data2, 0, 0);

            // Весь игровой ввод — в очередь IM, дренит sim-поток.
            input->HandleEvent(event);
        }
        SDL_Delay(16);
    }
    return 0;

}
