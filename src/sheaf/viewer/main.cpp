// SheafView: просмотр файлов Sheaf (docs/sheaf.md). Движок не нужен — только библиотека формата,
// SDL и ImGui. Запуск: SheafView [файл.sheaf]; файл можно перетащить в окно или открыть по Ctrl+O.
#include "Views.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <filesystem>
#include <mutex>

namespace {

// Диалог открытия зовёт колбэк с любого потока: путь передаётся в основной цикл через мьютекс,
// а событие будит цикл, спящий в SDL_WaitEvent.
std::mutex  g_dialog_mutex;
std::string g_dialog_path;

void SDLCALL OnFileChosen(void*, const char* const* files, int)
{
    if (!files || !files[0]) return;
    {
        std::lock_guard lock(g_dialog_mutex);
        g_dialog_path = files[0];
    }
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    const char* font = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font, 17.0f);
    else {
        io.Fonts->AddFontDefault();
        SDL_Log("SheafView: нет '%s' — кириллица будет '?'", font);
    }
}

void RenderFrame(SDL_GPUDevice* dev, SDL_Window* win)
{
    ImGui::Render();
    ImDrawData* draw = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUTexture* target = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, win, &target, nullptr, nullptr) || !target) {
        SDL_SubmitGPUCommandBuffer(cb);   // окно свёрнуто — кадр не нужен
        return;
    }
    ImGui_ImplSDLGPU3_PrepareDrawData(draw, cb);
    SDL_GPUColorTargetInfo color{};
    color.texture     = target;
    color.clear_color = { 0.08f, 0.08f, 0.09f, 1.0f };
    color.load_op     = SDL_GPU_LOADOP_CLEAR;
    color.store_op    = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &color, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(draw, cb, rp);
    SDL_EndGPURenderPass(rp);
    SDL_SubmitGPUCommandBuffer(cb);
}

} // namespace

int main(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("SheafView", 1400, 900, SDL_WINDOW_RESIZABLE);
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC
                                             | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
    if (!win || !dev || !SDL_ClaimWindowForGPUDevice(dev, win)) { SDL_Log("SheafView: %s", SDL_GetError()); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLGPU(win);
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device            = dev;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
    init.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);
    LoadFonts();

    ViewerState state;
    if (argc > 1) OpenFile(state, argv[1]);
    if (state.model.IsOpen()) SDL_SetWindowTitle(win, ("SheafView — " + state.model.Path()).c_str());

    // Кадр рисуется только после ввода: несколько кадров подряд, чтобы ImGui догнал наведение и
    // анимации, потом сон до следующего события. Пока активно поле ввода — будим раз в полсекунды
    // ради мигания курсора.
    int  frames_left = 3;
    bool running     = true;
    while (running) {
        if (frames_left == 0) {
            if (io.WantTextInput) SDL_WaitEventTimeout(nullptr, 500);
            else                  SDL_WaitEvent(nullptr);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            frames_left = 3;
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) OpenFile(state, e.drop.data);
            if (e.type == SDL_EVENT_USER) {
                std::string path;
                {
                    std::lock_guard lock(g_dialog_mutex);
                    path.swap(g_dialog_path);
                }
                if (!path.empty()) OpenFile(state, path);
            }
            if (e.type == SDL_EVENT_DROP_FILE || e.type == SDL_EVENT_USER)
                if (state.model.IsOpen()) SDL_SetWindowTitle(win, ("SheafView — " + state.model.Path()).c_str());
        }
        if (frames_left > 0) --frames_left;

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        DrawViewer(state);
        RenderFrame(dev, win);

        if (state.open_requested) {
            state.open_requested = false;
            static const SDL_DialogFileFilter filters[] = { { "Sheaf", "sheaf" }, { "Все файлы", "*" } };
            SDL_ShowOpenFileDialog(OnFileChosen, nullptr, win, filters, 2, nullptr, false);
        }
    }

    SDL_WaitForGPUIdle(dev);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
