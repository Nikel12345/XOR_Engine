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

// «+ create entity» разворачивает форму: чекбоксы компонентов из реестра + поля выбранных.
// Черновик — НАСТОЯЩАЯ энтити в сцене "_staging" (created в Engine-ините, никогда не активна:
// дата-модули/батчи её не видят, UI-поток правит её монопольно). Компоненты рисует ui::
// DrawEntityComponents — ТОТ ЖЕ вид, что у инспектора, только цель не живая (EditTarget).
// Create = SaveScene(staging) → CreateEntityCmd в sim-поток (LoadScene тем же путём, что файл
// сцены, + пересборка батчей).
namespace {
    constexpr Entity kNoEntity = static_cast<Entity>(-1);

    // ---- Список сцен = ПОДПАПКИ корня сцен (kScenesRoot) ----
    // Имя папки и есть имя сцены (Engine::Save/LoadScene складывают путь из корня и имени),
    // поэтому перечисление каталога — это готовый список сцен, без отдельного реестра-файла.
    // Сканируем не каждый кадр: обход каталога — syscall'ы, а состав папок меняется редко
    // (наш же Save или правка снаружи). Раз в секунду — и список сам подхватывает новое.
    std::vector<std::string> g_scene_dirs;
    double g_scene_dirs_time = -1.0;   // время последнего скана (ImGui::GetTime), <0 — не сканировали

    void RescanSceneDirs()
    {
        g_scene_dirs.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(kScenesRoot, ec)) {
            if (e.is_directory(ec)) g_scene_dirs.push_back(e.path().filename().string());
        }
        // Порядок обхода каталога не определён — сортируем, чтобы строки не прыгали между кадрами.
        std::sort(g_scene_dirs.begin(), g_scene_dirs.end());
        g_scene_dirs_time = ImGui::GetTime();
    }

    bool   g_ce_open   = false;      // форма развёрнута
    Entity g_ce_entity = kNoEntity;  // staging-черновик
    std::set<std::string> g_ce_checked;   // выбранные имена компонентов

    // ---- Список энтити с виртуализацией (1М+ объектов) ----
    // ImGui рисует КАЖДУЮ строку — на 1М энтити это роняет FPS. Список живёт в дочернем окне
    // со скроллбаром, а ImGuiListClipper рисует только видимые строки (см. DrawHierarchy).
    constexpr int kEntWindow = 10;   // высота списка в строках (сколько видно разом; скролл — на остальные)

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
                    (*na->get_array<MaterialComponent>())[nrow].materials =
                        (*oa->get_array<MaterialComponent>())[orow].materials;
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
                // Тот же вид, что у инспектора: черновик — это НАСТОЯЩАЯ энтити, просто не живая
                // (kNoEntity по умолчанию в EditTarget) → правки идут прямо в колонки.
                DrawEntityComponents(EditTarget{ ctx }, arch, row);
            }
        }

        ImGui::Separator();
        // Целевая сцена — активная. Без неё имя пустое: создавать некуда, поэтому кнопка гаснет,
        // а не шлёт команду в никуда (сам обработчик её тоже отсеет по GetScene, но черновик при
        // этом был бы уже удалён — то есть форма «сработала бы» и потеряла набранное).
        const SceneName target = om->GetActiveSceneName();
        ImGui::BeginDisabled(g_ce_entity == kNoEntity || target.empty());
        if (ImGui::Button("Create")) {
            std::string json = om->SaveScene(stg);   // staging = ровно одна сущность
            ctx->GetInputManager()->PushCommand(CommandId::CreateEntity,
                new CreateEntityCmd{ target, std::move(json) });
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

    // Проверка ПЕРЕД блоком сцен, а не после него (как было): без активной сцены её имя —
    // пустая строка, и блок ниже отдавал её в ImGui как лейбл (он же ID виджета), а кнопкам
    // Save/Load — как имя сцены в команду. Панель в этом состоянии не рисует ничего, кроме
    // подписи: показывать нечего, а любое действие тут работало бы с несуществующей сценой.
    if (!scene) {
        ImGui::TextDisabled("No active scene");
        ImGui::End();
        return;
    }

    // ---- Сцены: строка на каждую папку в корне сцен + Save/Load ----
    if (ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const SceneName active = om->GetActiveSceneName();
        if (g_scene_dirs_time < 0.0 || ImGui::GetTime() - g_scene_dirs_time > 1.0) RescanSceneDirs();

        // Имя папки = имя сцены, поэтому в команду уходит оно, а не активная сцена: Load грузит
        // ИМЕННО эту папку (движок сам её активирует), Save пишет одноимённую сцену — то есть
        // осмысленно ровно для загруженной (иначе SaveScene сообщит, что сцены такой нет).
        InputManager* im = ctx->GetInputManager();
        for (const std::string& name : g_scene_dirs) {
            ImGui::PushID(name.c_str());
            if (ImGui::SmallButton("Load"))
                im->PushCommand(CommandId::LoadScene, new SceneIOCmd{ name, kScenesRoot });
            ImGui::SameLine();
            if (ImGui::SmallButton("Save"))
                im->PushCommand(CommandId::SaveScene, new SceneIOCmd{ name, kScenesRoot });
            ImGui::SameLine();
            ImGui::Selectable(name.c_str(), name == active);
            ImGui::PopID();
        }

        // Активная сцена, у которой папки ещё нет (ни разу не сохранялась), иначе её нечем
        // сохранить: список показывает каталог, а её там нет. Save заведёт папку по её имени.
        if (std::find(g_scene_dirs.begin(), g_scene_dirs.end(), active) == g_scene_dirs.end()) {
            ImGui::PushID(active.c_str());
            if (ImGui::SmallButton("Save")) {
                im->PushCommand(CommandId::SaveScene, new SceneIOCmd{ active, kScenesRoot });
                g_scene_dirs_time = -1.0;   // папка вот-вот появится — пересканировать сразу
            }
            ImGui::SameLine();
            ImGui::Selectable(active.c_str(), true);
            ImGui::PopID();
        }
    }

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

        // АРХЕТИПНЫЙ доступ вместо пер-энтити сканов. Сигнатура (Positions+Material+Model)
        // и фильтры (EditorHidden/UI) — свойства АРХЕТИПА, у всех его строк они одинаковы,
        // проверять их на каждой энтити бессмысленно. Собираем диапазоны подходящих архетипов
        // с префикс-суммой: счёт = сумма размеров (O(архетипов)), а клиппер адресует видимую
        // строку НАПРЯМУЮ через arch->entities[row - base] (O(видимых строк) ~ 10).
        // Старый вариант гнал 2+ ПОЛНЫХ прохода по 1М энтити с двумя Has<> на каждую
        // (find в entity_to_archetype на 1М записей + get_array по type_index) КАЖДЫЙ кадр —
        // сотни мс на UI-потоке, панель роняла FPS на порядок при 10 видимых строках.
        struct EntRange { Archetype* arch; int base; };   // base = префикс-сумма строк до архетипа
        std::vector<EntRange> ranges;
        int total = 0;
        for (auto& [sig, arch] : scene->archetypes) {
            if (!arch.get_array<Positions>() || !arch.get_array<MaterialComponent>()
                || !arch.get_array<ModelComponent>()) continue;
            if (arch.get_array<EditorHiddenComponent>()) continue;
            if (arch.get_array<UIComponent>()) continue;   // UI живёт в своей вкладке (дерево Yoga), не тут
            if (arch.entities.empty()) continue;
            ranges.push_back({ &arch, total });
            total += static_cast<int>(arch.entities.size());
        }
        ImGui::Text("entities: %d", total);   // живой счётчик (ASCII: шрифт без кириллицы)

        // Список — в дочернем окне со СВОИМ скроллбаром (как на панелях). ImGuiListClipper
        // рисует ТОЛЬКО видимые строки (виртуализация на 1М), но держит полную высоту списка,
        // поэтому скроллбар честный. Строка row → архетип линейным поиском по ranges
        // (их единицы), внутри — прямой индекс.
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const int   rows  = std::max(1, std::min(total, kEntWindow));   // сколько строк видно разом
        ImGui::BeginChild("ent_list", ImVec2(0.0f, row_h * rows + ImGui::GetStyle().FramePadding.y * 2.0f), true);

        ImGuiListClipper clipper;
        clipper.Begin(total, row_h);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const EntRange* r = nullptr;   // ranges упорядочены по base → первый вмещающий
                for (const EntRange& rr : ranges)
                    if (row < rr.base + static_cast<int>(rr.arch->entities.size())) { r = &rr; break; }
                if (!r) break;                 // за концом списка (страховка от рассинхрона клиппера)
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

    // ---- UI (дерево Yoga: узлы = редактируемые объекты, не энтити; энтити — производные Emit) ----
    if (ImGui::CollapsingHeader("UI", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (UI_Yoga* yg = ctx->GetUIYoga()) {
            const UI_Yoga::Node root = yg->RootNode();
            if (root == UI_Yoga::kInvalid) ImGui::TextDisabled("(no UI tree)");
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
