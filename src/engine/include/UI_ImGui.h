#pragma once
#include "imgui.h"
#include "ObjectManager.h"
#include "CameraManager.h"

class EngineContext;

class UI_ImGui
{
public:
    static void Iterate(EngineContext* ctx);

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