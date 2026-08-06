#pragma once
#include <cstdint>
#include <vector>
#include <SDL3/SDL_events.h>

// Заголовку хватает forward-декларации: только статические методы с EngineContext*.
// imgui/ObjectManager/CameraManager тянут сами UI_*.cpp — по факту использования.
class EngineContext;

class UI_ImGui
{
public:
    static void Iterate(EngineContext* ctx);

    // ── Обмен выбором с игрой (вызывать с sim-потока). Выбор редактора сейчас — один
    // энтити; множественное число в имени — на вырост API (список длины 0/1).
    // Get ЗАБИРАЕТ выбор: возвращает выбранные энтити и снимает выделение — Inspector и
    // гизмо гаснут сами (рисуются только при выборе-энтити). Set — отдаёт выделение обратно.
    // Потокобезопасность: трогаются ТОЛЬКО POD-поля выбора (kind — «ворота» чтения, entity),
    // строка name не задевается; поля 4-байтовые выровненные, гонка с UI-потоком в худшем
    // случае сдвигает выбор на кадр — для редактора приемлемо. uint32_t = Entity.
    static std::vector<uint32_t> GetSelectedEntities();
    static void SetSelectedEntities(const std::vector<uint32_t>& entities);

    // ── Фасад ImGui для игры: движок — библиотека, а редактор в ней опционален, поэтому
    // игровой код не должен знать ни про imgui.h, ни про ImGuiIO. Когда редактор уедет под
    // рантайм-флаг, WantCapture* просто вернут false, а ProcessEvent станет no-op — игра
    // не изменится ни строкой. Гонка та же, что была при прямом чтении ImGuiIO: кадр ImGui
    // идёт на рендер-потоке, спрашивает игра с sim — ответ отстаёт максимум на кадр.
    static bool WantCaptureMouse();
    static bool WantCaptureKeyboard();
    // SDL-событие в бэкенд ImGui. Зовётся из цикла событий приложения (main-поток).
    static void ProcessEvent(const SDL_Event& event);

private:
    // Хост-докспейс поверх вьюпорта + первичная раскладка панелей (Hierarchy слева,
    // Inspector справа, Assets снизу). Центральная нода прозрачна — сквозь неё видна 3D-сцена.
    static void SetupDockspace();
    // Левая панель: сцены + группы объектов (Entities / Lights / Cameras). Только ВЫБОР
    // (клик → Selection); саму правку показывает Inspector.
    static void DrawHierarchy(EngineContext* ctx);
    // Правая панель: произвольная инфа по текущему выбору (энтити/свет/камера/материал/…),
    // сюда перенесена вся правка, что раньше была размазана по панелям.
    static void DrawInspector(EngineContext* ctx);
    // Нижняя панель: браузер ассетов (вкладки Materials/Textures/Models) — плитки-квадраты
    // с подписью-именем; клик → Selection, правка уходит в Inspector.
    static void DrawAssetBrowser(EngineContext* ctx);
    // Стрелки трансформации (ImGuizmo) для выбранной сущности. Рисуется ВНЕ окна-панели
    // (поверх сцены) и шлёт правку в sim-поток командой SetTransform.
    static void DrawGizmo(EngineContext* ctx);
};