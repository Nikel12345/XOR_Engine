#pragma once
#include <cstdint>
#include <vector>
#include <SDL3/SDL_events.h>

class EngineContext;
struct SDL_Window;
struct SDL_GPUDevice;

class UI_ImGui
{
public:
    static void Init(SDL_Window* win, SDL_GPUDevice* dev);
    static void Shutdown();

    static void Iterate(EngineContext* ctx);

    // Звать с SIM-потока. Get ЗАБИРАЕТ выбор: возвращает выбранное и снимает выделение.
    // Трогаются только выровненные POD-поля, поэтому гонка сдвигает выбор максимум на кадр.
    static std::vector<uint32_t> GetSelectedEntities();
    static void SetSelectedEntities(const std::vector<uint32_t>& entities);

    // Кадр ImGui идёт на рендер-потоке, а спрашивает игра с sim: ответ отстаёт на кадр.
    static bool WantCaptureMouse();
    static bool WantCaptureKeyboard();
    // Звать с MAIN-потока, из цикла событий приложения.
    static void ProcessEvent(const SDL_Event& event);

private:
    static void SetupDockspace();
    static void DrawHierarchy(EngineContext* ctx);
    static void DrawInspector(EngineContext* ctx);
    static void DrawAssetBrowser(EngineContext* ctx);
    static void DrawGizmo(EngineContext* ctx);
};