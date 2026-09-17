#include "PCH.h"
#include "BaseComponents.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "imgui.h"
#include "ObjectManager.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "InputCommands.h"
#include "ComponentSerializer.h"
#include "UI_ComponentEditor.h"
#include "UI_Yoga.h"
#include <set>
#include <algorithm>
#include <filesystem>

using namespace ui;

// Черновик формы — НАСТОЯЩАЯ энтити в сцене "staging": она никогда не активна, дата-модули и
// батчи её не видят, поэтому UI-поток правит её монопольно. Create уходит командой в sim.
namespace {
    constexpr Entity kNoEntity = static_cast<Entity>(-1);

    std::vector<std::string> g_scene_dirs;
    double g_scene_dirs_time = -1.0;

    void RescanSceneDirs()
    {
        g_scene_dirs.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(kScenesRoot, ec)) {
            if (e.is_directory(ec)) g_scene_dirs.push_back(e.path().filename().string());
        }
        std::sort(g_scene_dirs.begin(), g_scene_dirs.end());
        g_scene_dirs_time = ImGui::GetTime();
    }

    bool   g_ce_open   = false;
    Entity g_ce_entity = kNoEntity;
    std::set<std::string> g_ce_checked;

    constexpr int kEntWindow = 10;

    std::vector<const ComponentSpec*> CheckedSpecs()
    {
        std::vector<const ComponentSpec*> out;
        for (const ComponentSpec& s : ComponentSpecRegistry::Get().All())
            if (g_ce_checked.count(s.name)) out.push_back(&s);
        return out;
    }

    void RebuildStaging(ObjectManager* om, SceneData* stg)
    {
        const Entity old_e = g_ce_entity;
        std::vector<const ComponentSpec*> specs = CheckedSpecs();
        if (specs.empty()) {
            if (old_e != kNoEntity) om->DeleteEntity(stg, old_e);
            g_ce_entity = kNoEntity;
            return;
        }
        const Entity ne = om->CreateEntityFromSpecs(stg, specs);
        if (old_e != kNoEntity) {
            Archetype* oa = stg->entity_to_archetype[old_e];
            const size_t orow = stg->entity_to_index[old_e];
            Archetype* na = stg->entity_to_archetype[ne];
            const size_t nrow = stg->entity_to_index[ne];
            for (const ComponentSpec* s : specs) {
                if (!oa->components.count(s->sig_type)) continue;
                for (const FieldSpec& f : s->fields) {
                    if (f.get_num && f.set_num)      f.set_num(*na, nrow, f.get_num(*oa, orow));
                    else if (f.get_str && f.set_str) f.set_str(*na, nrow, f.get_str(*oa, orow));
                }
                if (s->name == "Material")
                    (*na->get_array<MaterialComponent>())[nrow].materials =
                        (*oa->get_array<MaterialComponent>())[orow].materials;
            }
            om->DeleteEntity(stg, old_e);
        }
        g_ce_entity = ne;
    }

    bool AllowedInForm(const ComponentSpec& s) { return s.name != "Parent"; }

    void DrawCreateEntityForm(EngineContext* ctx, ObjectManager* om)
    {
        SceneData* stg = om->GetScene("staging");
        if (!stg) return;

        ImGui::BeginChild("create_entity_form", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

        ImGui::TextDisabled("Components");
        ImGui::PushID("ce_set");
        bool set_changed = false;
        for (const ComponentSpec& s : ComponentSpecRegistry::Get().All()) {
            if (!AllowedInForm(s)) continue;
            bool on = g_ce_checked.count(s.name) != 0;
            if (ImGui::Checkbox(s.name.c_str(), &on)) {
                if (on) g_ce_checked.insert(s.name); else g_ce_checked.erase(s.name);
                set_changed = true;
            }
        }
        ImGui::PopID();
        if (set_changed) RebuildStaging(om, stg);

        if (g_ce_entity != kNoEntity) {
            auto ait = stg->entity_to_archetype.find(g_ce_entity);
            auto iit = stg->entity_to_index.find(g_ce_entity);
            if (ait != stg->entity_to_archetype.end() && iit != stg->entity_to_index.end()) {
                Archetype& arch = *ait->second;
                const size_t row = iit->second;
                ImGui::Separator();
                DrawEntityComponents(EditTarget{ ctx }, arch, row);
            }
        }

        ImGui::Separator();
        const SceneName target = om->GetActiveSceneName();
        ImGui::BeginDisabled(g_ce_entity == kNoEntity || target.empty());
        if (ImGui::Button("Create")) {
            std::string json = om->SaveScene(stg);
            cmd::Push<CommandId::CreateEntity>(ctx->GetInputManager(), target, std::move(json));
            om->DeleteEntity(stg, g_ce_entity);
            g_ce_entity = kNoEntity;
            g_ce_open = false;
        }
        ImGui::EndDisabled();
        if (target.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(no active scene)"); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (g_ce_entity != kNoEntity) om->DeleteEntity(stg, g_ce_entity);
            g_ce_entity = kNoEntity;
            g_ce_open = false;
        }

        ImGui::EndChild();
    }
}

static void DrawUITree(UI_Yoga* yg, UI_Yoga::Node n)
{
    if (n == UI_Yoga::kInvalid) return;
    const uint32_t child_count = yg->ChildCount(n);
    const std::string label = yg->NodeLabel(n);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (child_count == 0) flags |= ImGuiTreeNodeFlags_Leaf;
    const bool selected = (g_sel.kind == SelKind::UINode && g_sel.ui_node == n);
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(n)), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (selected) g_sel = Selection{};
        else { g_sel = Selection{}; g_sel.kind = SelKind::UINode; g_sel.ui_node = n; }
    }
    if (open) {
        for (uint32_t i = 0; i < child_count; ++i)
            DrawUITree(yg, yg->ChildAt(n, i));
        ImGui::TreePop();
    }
}

void UI_ImGui::DrawHierarchy(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Hierarchy");

    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Separator();

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();

    if (!scene) {
        ImGui::TextDisabled("No active scene");
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const SceneName active = om->GetActiveSceneName();
        if (g_scene_dirs_time < 0.0 || ImGui::GetTime() - g_scene_dirs_time > 1.0) RescanSceneDirs();

        InputManager* im = ctx->GetInputManager();
        for (const std::string& name : g_scene_dirs) {
            ImGui::PushID(name.c_str());
            if (ImGui::SmallButton("Load"))
                cmd::Push<CommandId::LoadScene>(im, name, kScenesRoot);
            ImGui::SameLine();
            if (ImGui::SmallButton("Save"))
                cmd::Push<CommandId::SaveScene>(im, name, kScenesRoot);
            ImGui::SameLine();
            ImGui::Selectable(name.c_str(), name == active);
            ImGui::PopID();
        }

        if (std::find(g_scene_dirs.begin(), g_scene_dirs.end(), active) == g_scene_dirs.end()) {
            ImGui::PushID(active.c_str());
            if (ImGui::SmallButton("Save")) {
                cmd::Push<CommandId::SaveScene>(im, active, kScenesRoot);
                g_scene_dirs_time = -1.0;
            }
            ImGui::SameLine();
            ImGui::Selectable(active.c_str(), true);
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button(g_ce_open ? "- create entity" : "+ create entity")) {
            g_ce_open = !g_ce_open;
            if (g_ce_open) {
                if (g_ce_checked.empty())
                    g_ce_checked = { "Transform", "Model", "Material", "Draw" };
                if (g_ce_entity == kNoEntity) RebuildStaging(om, om->GetScene("staging"));
            }
        }
        if (g_ce_open) DrawCreateEntityForm(ctx, om);

        // Сигнатура и фильтры — свойства АРХЕТИПА, поэтому список собирается диапазонами, а не
        // сканом по энтити: пер-энтити проход на 1М стоил сотни мс на UI-потоке каждый кадр.
        struct EntRange { Archetype* arch; int base; };
        std::vector<EntRange> ranges;
        int total = 0;
        for (auto& [sig, arch] : scene->archetypes) {
            if (!arch.get_array<Positions>() || !arch.get_array<MaterialComponent>()
                || !arch.get_array<ModelComponent>()) continue;
            if (arch.get_array<EditorHiddenComponent>()) continue;
            if (arch.get_array<UIComponent>()) continue;
            if (arch.entities.empty()) continue;
            ranges.push_back({ &arch, total });
            total += static_cast<int>(arch.entities.size());
        }
        ImGui::Text("entities: %d", total);

        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const int   rows  = std::max(1, std::min(total, kEntWindow));
        ImGui::BeginChild("ent_list", ImVec2(0.0f, row_h * rows + ImGui::GetStyle().FramePadding.y * 2.0f), true);

        ImGuiListClipper clipper;
        clipper.Begin(total, row_h);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const EntRange* r = nullptr;
                for (const EntRange& rr : ranges)
                    if (row < rr.base + static_cast<int>(rr.arch->entities.size())) { r = &rr; break; }
                if (!r) break;
                Entity e = r->arch->entities[row - r->base];
                char label[32];
                snprintf(label, sizeof(label), "Entity %u", static_cast<unsigned>(e));
                bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
                if (ImGui::Selectable(label, selected)) {
                    if (selected) g_sel = Selection{};
                    else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
                }
            }
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("UI", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (UI_Yoga* yg = ctx->GetUIYoga()) {
            const UI_Yoga::Node root = yg->RootNode();
            if (root == UI_Yoga::kInvalid) ImGui::TextDisabled("(no UI tree)");
            else DrawUITree(yg, root);
        }
    }

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
