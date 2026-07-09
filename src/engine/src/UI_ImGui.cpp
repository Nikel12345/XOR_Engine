#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "imgui_internal.h"   // DockBuilder* — первичная раскладка доков (см. SetupDockspace)
#include "ImGuizmo.h"         // ImGuizmo::BeginFrame — кадровый пролог гизмо

// Панели живут в отдельных TU (UI_Hierarchy / UI_AssetBrowser / UI_Inspector), но методы —
// все члены UI_ImGui, а их общее состояние объявлено в UI_Internal.h. Тут — «якорь»: определения
// этого состояния + кадровый оркестратор (Iterate) + первичная раскладка доков (SetupDockspace).

namespace ui {
    Selection g_sel;
    bool      g_show_internal = false;
}

void UI_ImGui::Iterate(EngineContext* ctx)
{
    ImGuizmo::BeginFrame();

    SetupDockspace();       // хост-докспейс + первичная раскладка панелей
    DrawHierarchy(ctx);     // слева
    DrawInspector(ctx);     // справа
    DrawAssetBrowser(ctx);  // снизу

    // Гизмо — ПОСЛЕ панелей: живёт не в окне, а поверх сцены (в прозрачной центральной ноде).
    DrawGizmo(ctx);
}

void UI_ImGui::SetupDockspace()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // id версионируем: смена строки заставляет пересобрать дефолтную раскладку поверх уже
    // сохранённого imgui.ini (старый node-id там просто не найдётся → ветка ниже сработает).
    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpaceV2");

    // Первичная раскладка строится ОДИН раз и только если её не восстановил imgui.ini
    // (DockBuilderGetNode == null до первого DockSpaceOverViewport с этим id). Так дефолт
    // получаешь на чистом старте, а сохранённую раскладку не затираем.
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);

            // Bottom откалываем ПЕРВЫМ от всего докспейса → он во всю ширину; Left/Right
            // отрезаем уже от верхнего остатка → колонки стоят НАД нижней панелью, не до края.
            ImGuiID center = dockspace_id;
            ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.30f, nullptr, &center);
            ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.20f, nullptr, &center);
            ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);

            ImGui::DockBuilderDockWindow("Hierarchy", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Assets",    bottom);
            ImGui::DockBuilderFinish(dockspace_id);
        }
    }

    // PassthruCentralNode: центральная нода прозрачна и не ловит мышь → сквозь неё видно
    // 3D-сцену и работает вращение камеры перетаскиванием (как раньше при !WantCaptureMouse).
    ImGui::DockSpaceOverViewport(dockspace_id, vp, ImGuiDockNodeFlags_PassthruCentralNode);
}
