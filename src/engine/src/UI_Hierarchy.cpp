#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"      // IsInternalName — фильтр служебных ассетов в комбо формы
#include "imgui.h"           // раньше транзитивно из UI_ImGui.h
#include "ObjectManager.h"   // иерархия читает сцену
#include "EngineContext.h"
#include "InputManager.h"   // PushCommand + CommandId
#include "InputCommands.h"  // SceneIOCmd + CreateEntityCmd
#include "ComponentSerializer.h"   // реестр спецификаций — чекбоксы набора компонентов
#include "UI_ComponentEditor.h"    // generic-редактор полей staging-энтити
#include "ModelManager.h"          // комбо моделей в форме
#include "MaterialManager.h"       // комбо материалов в форме
#include "UI_Yoga.h"               // UI-вкладка: дерево Yoga как редактируемый объект
#include <set>

using namespace ui;

// ============ Форма создания энтити (staging) ============
// «+ create entity» разворачивает форму: чекбоксы компонентов из реестра + поля выбранных.
// Черновик — НАСТОЯЩАЯ энтити в сцене "_staging" (created в Engine-ините, никогда не активна:
// дата-модули/батчи её не видят, UI-поток правит её монопольно). Поля рисует тот же
// DrawComponentFields, что у инспектора. Create = SaveScene(staging) → CreateEntityCmd в
// sim-поток (LoadScene тем же путём, что файл сцены, + фиксап ассетов + пересборка батчей).
namespace {
    constexpr Entity kNoEntity = static_cast<Entity>(-1);

    bool   g_ce_open   = false;      // форма развёрнута
    Entity g_ce_entity = kNoEntity;  // staging-черновик
    std::set<std::string> g_ce_checked;   // выбранные имена компонентов

    // Выбранные спеки в порядке регистрации (порядок секций формы детерминирован).
    std::vector<const ComponentSpec*> CheckedSpecs()
    {
        std::vector<const ComponentSpec*> out;
        for (const ComponentSpec& s : ComponentSpecRegistry::Get().All())
            if (g_ce_checked.count(s.name)) out.push_back(&s);
        return out;
    }

    // Пересоздать черновик под новый набор, ПЕРЕНЕСЯ значения общих компонентов
    // (правки полей переживают тыканье чекбоксов). Миграции архетипов в ECS нет —
    // это create+copy+delete, что для staging-черновика и есть правильный примитив.
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
            // Копии — строго ДО DeleteEntity: тот делает swap_remove строки старого черновика,
            // и orow читал бы уже ужатые колонки (OOB). Указатель oa вставка нового архетипа
            // не инвалидирует (scene->archetypes — node-based std::map).
            for (const ComponentSpec* s : specs) {
                if (!oa->components.count(s->sig_type)) continue;   // компонент новый — дефолты
                for (const FieldSpec& f : s->fields) {
                    if (f.get_num && f.set_num)      f.set_num(*na, nrow, f.get_num(*oa, orow));
                    else if (f.get_str && f.set_str) f.set_str(*na, nrow, f.get_str(*oa, orow));
                }
                if (s->name == "Material")   // custom-компонент: схемных полей нет, тип известен UI
                    (*na->get_array<MaterialComponent>())[nrow].names =
                        (*oa->get_array<MaterialComponent>())[orow].names;
            }
            om->DeleteEntity(stg, old_e);
        }
        g_ce_entity = ne;
    }

    // Не даём форме Parent: ремап id при загрузке работает только внутри одного файла
    // (LoadScene, проход 2) — родитель из ЖИВОЙ сцены не зарезолвится. Родство — отдельная фича.
    bool AllowedInForm(const ComponentSpec& s) { return s.name != "Parent"; }

    void DrawCreateEntityForm(EngineContext* ctx, ObjectManager* om)
    {
        SceneData* stg = om->GetScene("_staging");
        if (!stg) return;

        ImGui::BeginChild("create_entity_form", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

        // ---- Набор компонентов ----
        // Свой ID-скоуп: лейблы чекбоксов совпадают с CollapsingHeader секций полей ниже
        // (оба «Model», «Draw»...) — без скоупа ImGui ругается на конфликт ID.
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

        // ---- Поля выбранных компонентов (черновик правится напрямую — staging невидим рендеру) ----
        if (g_ce_entity != kNoEntity) {
            auto ait = stg->entity_to_archetype.find(g_ce_entity);
            auto iit = stg->entity_to_index.find(g_ce_entity);
            if (ait != stg->entity_to_archetype.end() && iit != stg->entity_to_index.end()) {
                Archetype& arch = *ait->second;
                const size_t row = iit->second;
                ImGui::Separator();
                for (const ComponentSpec* s : CheckedSpecs()) {
                    const bool is_model    = (s->name == "Model");
                    const bool is_material = (s->name == "Material");
                    if (s->fields.empty() && !is_material) continue;   // теги — править нечего
                    if (!ImGui::CollapsingHeader(s->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

                    if (is_model) {
                        // В форме имя модели ВЫБИРАЕТСЯ (в отличие от ReadOnly у живой энтити):
                        // указатель зарезолвит хендлер CreateEntityCmd тем же путём, что загрузка.
                        ModelComponent& mdl = (*arch.get_array<ModelComponent>())[row];
                        if (ImGui::BeginCombo("model", mdl.name.empty() ? "(none)" : mdl.name.c_str())) {
                            if (ImGui::Selectable("(none)", mdl.name.empty())) mdl.name.clear();
                            for (auto& [nm, m] : ctx->GetModelManager()->GetModels()) {
                                if (!g_show_internal && IsInternalName(nm)) continue;
                                if (ImGui::Selectable(nm.c_str(), nm == mdl.name)) mdl.name = nm;
                            }
                            ImGui::EndCombo();
                        }
                    }
                    else if (is_material) {
                        // Список имён материалов; порядок = сабмеши модели (индекс сабмеша → материал).
                        MaterialComponent& mats = (*arch.get_array<MaterialComponent>())[row];
                        ImGui::TextDisabled("order = model submeshes");
                        for (size_t k = 0; k < mats.names.size(); ++k) {
                            ImGui::PushID(static_cast<int>(k));
                            std::string& cur = mats.names[k];
                            if (ImGui::BeginCombo("##mat", cur.empty() ? "(none)" : cur.c_str())) {
                                for (auto& [nm, m] : ctx->GetMaterialManager()->GetMaterials()) {
                                    if (!g_show_internal && IsInternalName(nm)) continue;
                                    if (ImGui::Selectable(nm.c_str(), nm == cur)) cur = nm;
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("-")) {
                                mats.names.erase(mats.names.begin() + k);
                                ImGui::PopID();
                                break;
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("+ material")) mats.names.emplace_back();
                    }
                    else {
                        DrawComponentFields(*s, arch, row);
                    }
                }
            }
        }

        // ---- Create / Cancel ----
        ImGui::Separator();
        ImGui::BeginDisabled(g_ce_entity == kNoEntity);
        if (ImGui::Button("Create")) {
            std::string json = om->SaveScene(stg);   // staging = ровно одна сущность
            ctx->GetInputManager()->PushCommand(CommandId::CreateEntity,
                new CreateEntityCmd{ om->GetActiveSceneName(), std::move(json) });
            om->DeleteEntity(stg, g_ce_entity);
            g_ce_entity = kNoEntity;
            g_ce_open = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (g_ce_entity != kNoEntity) om->DeleteEntity(stg, g_ce_entity);
            g_ce_entity = kNoEntity;
            g_ce_open = false;
        }

        ImGui::EndChild();
    }
} // namespace

// Левая панель: сцены + группы объектов (Entities / Lights / Cameras). Только ВЫБОР
// (клик → g_sel); саму правку показывает Inspector.
// Рекурсивный рендер дерева Yoga в UI-вкладке. Узел = объект дерева (не энтити); клик выбирает
// его (SelKind::UINode) — под будущий гизмо, двигающий его offset (узел + поддерево).
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
        // Кнопка-тумблер формы создания. Закрытие кнопкой НЕ сбрасывает черновик —
        // повторное открытие продолжает с того же места; сброс делает Cancel/Create.
        if (ImGui::Button(g_ce_open ? "- create entity" : "+ create entity")) {
            g_ce_open = !g_ce_open;
            if (g_ce_open) {
                if (g_ce_checked.empty())   // стартовый набор — типичный рисуемый
                    g_ce_checked = { "Transform", "Model", "Material", "Draw" };
                if (g_ce_entity == kNoEntity) RebuildStaging(om, om->GetScene("_staging"));
            }
        }
        if (g_ce_open) DrawCreateEntityForm(ctx, om);

        om->ForEach<Positions, MaterialComponent, ModelComponent>(scene,
            [&](Entity e, SoAElement<Positions>, MaterialComponent&, ModelComponent&)
        {
            if (om->Has<EditorHiddenComponent>(scene, e)) return;
            if (om->Has<UIComponent>(scene, e)) return;   // UI живёт в своей вкладке (дерево Yoga), не тут
            char label[32];
            snprintf(label, sizeof(label), "Entity %u", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Entity && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Entity; g_sel.entity = e; }
            }
        });
    }

    // ---- UI (дерево Yoga: узлы = редактируемые объекты, не энтити; энтити — производные Emit) ----
    if (ImGui::CollapsingHeader("UI", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (UI_Yoga* yg = ctx->GetUIYoga()) {
            const UI_Yoga::Node root = yg->RootNode();
            if (root == UI_Yoga::kInvalid) ImGui::TextDisabled("(нет UI-дерева)");
            else DrawUITree(yg, root);
        }
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
