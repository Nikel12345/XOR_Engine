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
    static void DrawCameraPanel(CameraManager* cameraManager);
    static void DrawObjectsPanel(EngineContext* ctx);
    static void DrawMaterialsPanel(EngineContext* ctx);
    static void DrawLightsPanel(ObjectManager* objectManager);
    // Стрелки трансформации (ImGuizmo) для выбранной в списке сущности. Рисуется ВНЕ
    // окна-панели (поверх сцены) и шлёт правку в sim-поток командой SetTransform.
    static void DrawGizmo(EngineContext* ctx);
};