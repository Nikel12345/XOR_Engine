#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "imgui.h"           // раньше транзитивно из UI_ImGui.h
#include "ObjectManager.h"   // иерархия читает сцену
#include "EngineContext.h"
#include "InputManager.h"   // PushCommand + CommandId
#include "InputCommands.h"  // SceneIOCmd

using namespace ui;

// Левая панель: сцены + группы объектов (Entities / Lights / Cameras). Только ВЫБОР
// (клик → g_sel); саму правку показывает Inspector.
void UI_ImGui::DrawHierarchy(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Hierarchy");

    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Separator();

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();

    // ---- Сцены (пока одна активная; список — задел) + Save/Load ----
    if (ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        SceneName active = om->GetActiveSceneName();
        ImGui::Selectable(active.c_str(), true);   // активная сцена

        // Путь — ПАПКА сцены (scene.scene + файлы ресурсов внутри), см. Engine::Save/LoadScene.
        if (ImGui::SmallButton("Save scene")) {
            ctx->GetInputManager()->PushCommand(CommandId::SaveScene,
                new SceneIOCmd{ active, "saved_scene" });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Load scene")) {
            ctx->GetInputManager()->PushCommand(CommandId::LoadScene,
                new SceneIOCmd{ active, "saved_scene" });
        }
    }

    if (!scene) { ImGui::End(); return; }

    // ---- Обычные энтити (с моделью/материалом; debug-рамки скрыты тегом) ----
    if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen))
    {
        om->ForEach<Positions, MaterialComponent, ModelComponent>(scene,
            [&](Entity e, SoAElement<Positions>, MaterialComponent&, ModelComponent&)
        {
            if (om->Has<EditorHiddenComponent>(scene, e)) return;
            char label[32];
            snprintf(label, sizeof(label), "Entity %u", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
            }
        });
    }

    // ---- Света (все типы; выбор → Inspector определит тип по Has<>) ----
    if (ImGui::CollapsingHeader("Lights"))
    {
        om->ForEach<Positions, SpotLightComponent>(scene,
            [&](Entity e, SoAElement<Positions>, SpotLightComponent&)
        {
            char label[32]; snprintf(label, sizeof(label), "Spot (e%u)", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
            }
        });
        om->ForEach<Positions, SphereLightComponent>(scene,
            [&](Entity e, SoAElement<Positions>, SphereLightComponent&)
        {
            char label[32]; snprintf(label, sizeof(label), "Sphere (e%u)", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
            }
        });
        om->ForEach<DirectLightComponent>(scene,
            [&](Entity e, DirectLightComponent&)
        {
            char label[32]; snprintf(label, sizeof(label), "Directional (e%u)", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
            }
        });
    }

    // ---- Камеры (пока активная) ----
    if (ImGui::CollapsingHeader("Cameras"))
    {
        bool selected = (g_sel.kind == SelKind::Camera);
        if (ImGui::Selectable("Camera 0", selected)) {
            if (selected) g_sel = Selection{};
            else { g_sel = Selection{}; g_sel.kind = SelKind::Camera; g_sel.index = 0; }
        }
    }

    ImGui::End();
}
