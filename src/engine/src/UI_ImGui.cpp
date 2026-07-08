#include "PCH.h"
#include "UI_ImGui.h"
#include "imgui_internal.h"   // DockBuilder* — первичная раскладка доков (см. SetupDockspace)
#include "EngineContext.h"
#include "InputManager.h"
#include "MaterialParams.h"   // раскладки факторов: разбор params по полям в инспекторе материала
#include "ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>            // memcpy матрицы в payload команды
#include <algorithm>          // std::max / std::sort
#include <vector>             // списки имён для комбобоксов/плиток

namespace {
    // ---- Выделение редактора: что сейчас показывает Inspector ----
    // Раньше выбор был только Entity (g_selected). Теперь общий: клик в Hierarchy или по
    // плитке ассета кладёт сюда «вид + идентификатор», а Inspector по нему решает, что рисовать.
    enum class SelKind { None, Entity, Light, Camera, Material, Texture, Model };
    struct Selection {
        SelKind kind = SelKind::None;
        Entity  entity = static_cast<Entity>(-1);  // для Entity / Light (свет — тоже сущность)
        int     index = -1;                        // для Camera (порядковый)
        std::string name;                          // для Material / Texture / Model
    };
    Selection g_sel;

    constexpr Entity GIZMO_NONE = static_cast<Entity>(-1);
    // Полупрозрачный фон панелей (пустые места не глухо-чёрные, сквозь чуть видно сцену).
    constexpr float kPanelBgAlpha = 0.0f;

    // Текущий режим стрелок. Переключается радиокнопками в Inspector.
    ImGuizmo::OPERATION g_gizmo_op   = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      g_gizmo_mode = ImGuizmo::WORLD;

    // Positions хранит матрицу row-major (трансляция в w/d/h = индексы 3,7,11), а glm/
    // ImGuizmo ждут column-major — поэтому собираем glm::mat4 транспонируя поэлементно.
    glm::mat4 ReadPositionsMatrix(const Positions& P, size_t i)
    {
        glm::mat4 m;                                 // m[col][row]
        m[0][0] = P.x[i]; m[1][0] = P.y[i]; m[2][0] = P.z[i]; m[3][0] = P.w[i];
        m[0][1] = P.a[i]; m[1][1] = P.b[i]; m[2][1] = P.c[i]; m[3][1] = P.d[i];
        m[0][2] = P.e[i]; m[1][2] = P.f[i]; m[2][2] = P.g[i]; m[3][2] = P.h[i];
        m[0][3] = P.i[i]; m[1][3] = P.j[i]; m[2][3] = P.k[i]; m[3][3] = P.l[i];
        return m;
    }

    // Одна плитка-затычка браузера ассетов: квадрат (Selectable) + имя под ним, всё в группе
    // (чтобы SameLine переносил их как единое целое). true при клике.
    bool AssetTile(const char* name, bool selected, float size)
    {
        ImGui::BeginGroup();
        ImGui::PushID(name);
        bool clicked = ImGui::Selectable("##sq", selected, ImGuiSelectableFlags_None, ImVec2(size, size));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size);   // подпись не шире плитки
        ImGui::TextWrapped("%s", name);
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        ImGui::EndGroup();
        return clicked;
    }

    // TextureSlotRole (enum слота) → строка для ImGui. Явный switch, а не рефлексия: набор ролей
    // фиксирован и мал, а Custom0=1000 сломал бы range-scan magic_enum/source_location-трюков.
    const char* RoleName(TextureSlotRole r)
    {
        switch (r) {
        case TextureSlotRole::Albedo:   return "Albedo";
        case TextureSlotRole::Normal:   return "Normal";
        case TextureSlotRole::ORM:      return "ORM";        // == MetallicRoughness (алиас)
        case TextureSlotRole::Emissive: return "Emissive";
        case TextureSlotRole::Custom0:  return "Custom0";
        case TextureSlotRole::Custom1:  return "Custom1";
        case TextureSlotRole::Custom2:  return "Custom2";
        case TextureSlotRole::Custom3:  return "Custom3";
        case TextureSlotRole::Custom4:  return "Custom4";
        case TextureSlotRole::Custom5:  return "Custom5";
        case TextureSlotRole::Custom6:  return "Custom6";
        case TextureSlotRole::Custom7:  return "Custom7";
        default:                        return "Role";
        }
    }

    // ================= Inspector-блоки (перенос существующих виджетов как есть) =================

    void InspectEntity(EngineContext* ctx, ObjectManager* om, SceneData* scene, Entity e)
    {
        if (!om->Has<Positions>(scene, e)) { ImGui::TextUnformatted("Entity has no transform."); return; }
        SoAElement<Positions> el = om->GetComponent<Positions>(scene, e);
        Positions& P = el.container();
        size_t i = el.i();

        ImGui::Text("Entity %u", static_cast<unsigned>(e));

        float pos[3] = { P.w[i], P.d[i], P.h[i] };
        if (ImGui::DragFloat3("Offset", pos, 0.05f))
        {
            P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2];
        }

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
        {
            // UI сам ECS не мутирует — кладёт команду в очередь (Entity пакуется прямо в указатель),
            // sim-поток исполнит в MainIterate. После удаления снимаем выбор.
            ctx->GetInputManager()->PushCommand(CommandId::DeleteEntity,
                reinterpret_cast<const void*>(static_cast<uintptr_t>(e)));
            g_sel = Selection{};
        }
        ImGui::PopStyleColor(3);

        // Дети владельца — debug-рамки коллайдеров (помечены EditorHiddenComponent, потому в списки
        // не попадают). Галочка visible каждого: снятие прячет рамку из рендера, НЕ удаляя энтити.
        auto kids_it = scene->children.find(e);
        if (kids_it != scene->children.end() && !kids_it->second.empty())
        {
            ImGui::SeparatorText("Debug colliders");
            for (Entity c : kids_it->second)
            {
                if (!om->Has<DrawComponent>(scene, c)) continue;
                bool visible = om->GetComponent<DrawComponent>(scene, c).visible;

                char clabel[40];
                snprintf(clabel, sizeof(clabel), "visible (collider %u)", static_cast<unsigned>(c));
                if (ImGui::Checkbox(clabel, &visible))
                {
                    const uintptr_t packed = static_cast<uintptr_t>(c)
                        | (visible ? (static_cast<uintptr_t>(1) << 32) : static_cast<uintptr_t>(0));
                    ctx->GetInputManager()->PushCommand(CommandId::HideEntity,
                        reinterpret_cast<const void*>(packed));
                }
            }
        }
    }

    void InspectMaterial(EngineContext* ctx, const std::string& matName, Material* mat)
    {
        if (!mat) { ImGui::TextUnformatted("Material not found."); return; }

        // ---- Слоты текстур: свап через КОМАНДУ (правит имя в Material::textures[role]) +
        //      пересборку батчей. НЕ in-place: в батч запечён разрешённый UVL, его надо пересчитать.
        ImGui::SeparatorText("Textures");
        std::vector<std::string> texNames;                       // значения комбобокса — по алфавиту
        for (auto& [n, h] : ctx->GetTextureManager()->GetTextureHandles()) texNames.push_back(n);
        std::sort(texNames.begin(), texNames.end());

        std::vector<TextureSlotRole> roles;                      // слоты материала в стабильном порядке
        for (auto& [role, name] : mat->textures) roles.push_back(role);
        std::sort(roles.begin(), roles.end(),
            [](TextureSlotRole a, TextureSlotRole b){ return static_cast<uint32_t>(a) < static_cast<uint32_t>(b); });

        for (TextureSlotRole role : roles) {
            const std::string current = mat->textures[role];
            ImGui::PushID(static_cast<int>(role));
            if (ImGui::BeginCombo(RoleName(role), current.c_str())) {   // RoleName: enum слота → строка
                for (const std::string& tn : texNames) {
                    bool is_cur = (tn == current);
                    if (ImGui::Selectable(tn.c_str(), is_cur) && !is_cur)
                        ctx->GetInputManager()->PushCommand(CommandId::SetMaterialTexture,
                            new SetMaterialTextureCmd{ matName, static_cast<uint32_t>(role), tn });
                    if (is_cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }

        // ---- Params ----
        // Смена ТИПА params (kind) = SetMaterialParams<T> с дефолтным блобом, ровно как в коде.
        // IN-PLACE, без ребилда: RenderManager читает params->data()/size() ЖИВО по указателю на
        // вектор (адрес &Material::params стабилен при resize). Kind — фиксированный enum, перечисляем
        // напрямую. ВНИМАНИЕ: тип должен совпадать с раскладкой MaterialBlock шейдера материала —
        // иначе в cbuffer уедет чужой блоб (позиционное доверие). Меняй осознанно под шейдер.
        ImGui::SeparatorText("Params");
        struct KindOpt { MaterialParamsKind kind; const char* label; };
        static const KindOpt kKinds[] = {
            { MaterialParamsKind::None,        "None" },
            { MaterialParamsKind::Opaque,      "Opaque" },
            { MaterialParamsKind::Transparent, "Transparent" },
        };
        const char* curLabel = "?";
        for (auto& k : kKinds) if (k.kind == mat->params_kind) curLabel = k.label;
        if (ImGui::BeginCombo("Kind", curLabel)) {
            for (auto& k : kKinds) {
                bool is_cur = (k.kind == mat->params_kind);
                if (ImGui::Selectable(k.label, is_cur) && !is_cur) {
                    switch (k.kind) {
                    case MaterialParamsKind::Opaque:      ctx->SetMaterialParams(mat, OpaqueMaterialParams{}); break;
                    case MaterialParamsKind::Transparent: ctx->SetMaterialParams(mat, TransparentMaterialParams{}); break;
                    default: mat->params.clear(); mat->params_kind = MaterialParamsKind::None; break;
                    }
                }
                if (is_cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Значения текущего типа (тоже IN-PLACE).
        if (mat->params.empty()) { ImGui::TextDisabled("(no params)"); return; }
        switch (mat->params_kind)
        {
        case MaterialParamsKind::Opaque:
        {
            auto* p = reinterpret_cast<OpaqueMaterialParams*>(mat->params.data());
            ImGui::ColorEdit3("Base Color", p->baseColor);
            ImGui::ColorEdit3("Emissive", p->emissive);
            ImGui::SliderFloat("Emissive Strength", &p->emissiveStrength, 0.0f, 8.0f);
            ImGui::SliderFloat("Metallic", &p->metallic, 0.0f, 1.0f);
            ImGui::SliderFloat("Roughness", &p->roughness, 0.0f, 1.0f);
            break;
        }
        case MaterialParamsKind::Transparent:
        {
            auto* p = reinterpret_cast<TransparentMaterialParams*>(mat->params.data());
            ImGui::SliderFloat("Alpha", &p->alpha, 0.0f, 1.0f);
            break;
        }
        default:
        {
            float* f = reinterpret_cast<float*>(mat->params.data());
            const size_t n = mat->params.size() / sizeof(float);
            for (size_t k = 0; k < n; ++k)
            {
                char l[16];
                snprintf(l, sizeof(l), "p%u", static_cast<unsigned>(k));
                ImGui::SliderFloat(l, &f[k], 0.0f, 1.0f);
            }
            break;
        }
        }
    }

    void InspectSpotLight(Positions& P, size_t i, SpotLightComponent& light)
    {
        auto& d = light.light_data;
        bool changed = false;

        ImGui::SeparatorText("Transform");
        float pos[3] = { P.w[i], P.d[i], P.h[i] };
        if (ImGui::DragFloat3("Position", pos, 0.05f)) { P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2]; }
        float dir[3] = { d.dir_x, d.dir_y, d.dir_z };
        if (ImGui::DragFloat3("Direction", dir, 0.01f, -1.0f, 1.0f))
        {
            d.dir_x = dir[0]; d.dir_y = dir[1]; d.dir_z = dir[2]; changed = true;
        }

        ImGui::SeparatorText("Cone");
        changed |= ImGui::SliderAngle("Angle", &d.source_angle, 1.0f, 89.0f);
        changed |= ImGui::DragFloat("Source Radius", &d.source_radius, 0.01f, 0.0f, FLT_MAX);

        ImGui::SeparatorText("Color");
        changed |= ImGui::ColorEdit3("RGB", &d.r);

        ImGui::SeparatorText("Falloff");
        changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);
        changed |= ImGui::DragFloat("Attenuation", &d.attenuation, 0.05f, 0.0f, FLT_MAX);

        d.ResolveDistance();
        ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());
        if (changed) light.needsUpdate = true;
    }

    void InspectSphereLight(Positions& P, size_t i, SphereLightComponent& light)
    {
        auto& d = light.light_data;
        bool changed = false;

        ImGui::SeparatorText("Transform");
        float pos[3] = { P.w[i], P.d[i], P.h[i] };
        if (ImGui::DragFloat3("Position", pos, 0.05f)) { P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2]; }

        ImGui::SeparatorText("Shape");
        changed |= ImGui::DragFloat("Radius", &d.source_radius, 0.01f, 0.0f, FLT_MAX);

        ImGui::SeparatorText("Color");
        changed |= ImGui::ColorEdit3("RGB", &d.r);

        ImGui::SeparatorText("Falloff");
        changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);
        changed |= ImGui::DragFloat("Attenuation", &d.attenuation, 0.05f, 0.0f, FLT_MAX);

        d.ResolveDistance();
        ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());
        if (changed) light.needsUpdate = true;
    }

    void InspectDirectLight(DirectLightComponent& light)
    {
        auto& d = light.light_data;
        bool changed = false;

        ImGui::SeparatorText("Direction");
        changed |= ImGui::DragFloat3("Dir", &d.dir_x, 0.01f, -1.0f, 1.0f);

        ImGui::SeparatorText("Color");
        changed |= ImGui::ColorEdit3("RGB", &d.r);
        changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);

        ImGui::SeparatorText("Shadow Cascades");
        changed |= ImGui::DragFloat3("Center", &d.center_x, 0.05f);
        changed |= ImGui::DragFloat("Half Extent (c0)", &d.half_extent, 0.1f, 0.01f, FLT_MAX);
        changed |= ImGui::DragFloat("Half Depth (c0)", &d.half_depth, 0.1f, 0.01f, FLT_MAX);
        changed |= ImGui::DragFloat("Cascade Ratio", &d.cascade_ratio, 0.05f, 1.0f, FLT_MAX);
        if (ImGui::InputInt("Cascade Count", &d.cascade_count)) {
            if (d.cascade_count < 1) d.cascade_count = 1;
            if (d.cascade_count > DirectLightComponent::DirectLightData::MAX_CASCADES)
                d.cascade_count = DirectLightComponent::DirectLightData::MAX_CASCADES;
            changed = true;
        }
        for (int c = 0; c < d.cascade_count; ++c) {
            float he = d.CascadeExtent(c);
            float dp = d.CascadeDepth(c);
            ImGui::Text("  c%d: %.1f x %.1f, depth %.1f, texel %.4f",
                c, 2.0f * he, 2.0f * he, 2.0f * dp, (2.0f * he) / 1024.0f);
        }
        if (changed) light.needsUpdate = true;
    }

    void InspectCamera(Camera* cam)
    {
        if (!cam) { ImGui::TextUnformatted("No active camera."); return; }
        glm::vec3 pos = cam->GetPosition();
        glm::vec3 tgt = cam->GetTarget();
        if (ImGui::DragFloat3("Cam Position", &pos.x, 0.05f))
            cam->SetPosition(pos);
        if (ImGui::DragFloat3("Cam Target", &tgt.x, 0.05f))
            cam->SetView(pos, tgt, glm::vec3(0.0f, 1.0f, 0.0f));
    }
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

        if (ImGui::SmallButton("Save scene")) {
            ctx->GetInputManager()->PushCommand(CommandId::SaveScene,
                new SceneIOCmd{ active, "saved_scene.scene" });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Load scene")) {
            ctx->GetInputManager()->PushCommand(CommandId::LoadScene,
                new SceneIOCmd{ active, "saved_scene.scene" });
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
            bool selected = (g_sel.kind == SelKind::Light && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Light; g_sel.entity = e; }
            }
        });
        om->ForEach<Positions, SphereLightComponent>(scene,
            [&](Entity e, SoAElement<Positions>, SphereLightComponent&)
        {
            char label[32]; snprintf(label, sizeof(label), "Sphere (e%u)", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Light && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Light; g_sel.entity = e; }
            }
        });
        om->ForEach<DirectLightComponent>(scene,
            [&](Entity e, DirectLightComponent&)
        {
            char label[32]; snprintf(label, sizeof(label), "Directional (e%u)", static_cast<unsigned>(e));
            bool selected = (g_sel.kind == SelKind::Light && g_sel.entity == e);
            if (ImGui::Selectable(label, selected)) {
                if (selected) g_sel = Selection{};
                else { g_sel = Selection{}; g_sel.kind = SelKind::Light; g_sel.entity = e; }
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

void UI_ImGui::DrawInspector(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Inspector");

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();

    // Панелька гизмо — глобальный режим стрелок (относится к выбранной сущности).
    ImGui::TextUnformatted("Gizmo:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Move",    g_gizmo_op == ImGuizmo::TRANSLATE)) g_gizmo_op = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate",  g_gizmo_op == ImGuizmo::ROTATE))    g_gizmo_op = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale",   g_gizmo_op == ImGuizmo::SCALE))     g_gizmo_op = ImGuizmo::SCALE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Uniform", g_gizmo_op == ImGuizmo::SCALEU))    g_gizmo_op = ImGuizmo::SCALEU;
    ImGui::SameLine();
    if (ImGui::SmallButton("Deselect")) g_sel = Selection{};
    ImGui::Separator();

    switch (g_sel.kind)
    {
    case SelKind::Entity:
        if (scene) InspectEntity(ctx, om, scene, g_sel.entity);
        break;

    case SelKind::Light:
        if (scene) {
            Entity e = g_sel.entity;
            if (om->Has<SpotLightComponent>(scene, e) && om->Has<Positions>(scene, e)) {
                SoAElement<Positions> el = om->GetComponent<Positions>(scene, e);
                InspectSpotLight(el.container(), el.i(), om->GetComponent<SpotLightComponent>(scene, e));
            }
            else if (om->Has<SphereLightComponent>(scene, e) && om->Has<Positions>(scene, e)) {
                SoAElement<Positions> el = om->GetComponent<Positions>(scene, e);
                InspectSphereLight(el.container(), el.i(), om->GetComponent<SphereLightComponent>(scene, e));
            }
            else if (om->Has<DirectLightComponent>(scene, e)) {
                InspectDirectLight(om->GetComponent<DirectLightComponent>(scene, e));
            }
            else ImGui::TextUnformatted("Light no longer exists.");
        }
        break;

    case SelKind::Camera:
        InspectCamera(ctx->GetCameraManager()->GetActiveCamera());
        break;

    case SelKind::Material:
        ImGui::Text("Material: %s", g_sel.name.c_str());
        ImGui::Separator();
        InspectMaterial(ctx, g_sel.name, ctx->GetMaterialManager()->GetMaterial(g_sel.name));
        break;

    case SelKind::Texture:
        ImGui::Text("Texture: %s", g_sel.name.c_str());
        ImGui::TextDisabled("(editing not implemented yet)");
        break;

    case SelKind::Model:
        ImGui::Text("Model: %s", g_sel.name.c_str());
        ImGui::TextDisabled("(editing not implemented yet)");
        break;

    default:
        ImGui::TextDisabled("Nothing selected.");
        break;
    }

    ImGui::End();
}

void UI_ImGui::DrawAssetBrowser(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Assets");

    // Фильтр служебных ассетов: имена на "_" (напр. "_NoTextureDummy") прячем по умолчанию.
    static bool show_internal = false;
    ImGui::Checkbox("Show internal (_)", &show_internal);
    ImGui::Separator();

    const float tile = 64.0f, pad = 8.0f;

    if (ImGui::BeginTabBar("AssetTabs"))
    {
        // Общая раскладка плиток: переносим ряд, когда следующая не влезает по ширине.
        auto tiles = [&](SelKind kind, auto&& for_each_name)
        {
            float avail = ImGui::GetContentRegionAvail().x;
            int per_row = std::max(1, static_cast<int>(avail / (tile + pad)));
            int col = 0;
            for_each_name([&](const std::string& name)
            {
                if (!show_internal && !name.empty() && name[0] == '_') return;   // фильтр служебных
                bool selected = (g_sel.kind == kind && g_sel.name == name);
                if (AssetTile(name.c_str(), selected, tile)) {
                    if (selected) g_sel = Selection{};                                 // повторный клик — снять
                    else { g_sel = Selection{}; g_sel.kind = kind; g_sel.name = name; }
                }
                if (++col % per_row != 0) ImGui::SameLine();
            });
        };

        if (ImGui::BeginTabItem("Materials")) {
            tiles(SelKind::Material, [&](auto&& emit) {
                for (auto& [name, mat] : ctx->GetMaterialManager()->GetMaterials()) emit(name);
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            tiles(SelKind::Texture, [&](auto&& emit) {
                for (auto& [name, h] : ctx->GetTextureManager()->GetTextureHandles()) emit(name);
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Models")) {
            tiles(SelKind::Model, [&](auto&& emit) {
                for (auto& [name, m] : ctx->GetModelManager()->GetModels()) emit(name);
            });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void UI_ImGui::DrawGizmo(EngineContext* ctx)
{
    if (g_sel.kind != SelKind::Entity) return;
    Entity selected = g_sel.entity;

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    // Сущность могли удалить через очередь команд — снимаем выбор и выходим.
    if (!om->Has<Positions>(scene, selected)) { g_sel = Selection{}; return; }

    Camera* cam = ctx->GetCameraManager()->GetActiveCamera();
    if (!cam) return;
    glm::mat4 view = cam->GetView();
    glm::mat4 proj = cam->GetProj();

    SoAElement<Positions> el = om->GetComponent<Positions>(scene, selected);
    Positions& P = el.container();
    const size_t i = el.i();

    glm::mat4 model = ReadPositionsMatrix(P, i);
    const glm::mat4 model_before = model;   // матрица ДО манипуляции (для разворота поворота)

    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    float delta[16];   // приращение этого кадра (мировое), нужно для разворота вращения
    if (ImGuizmo::Manipulate(glm::value_ptr(view),
                             glm::value_ptr(proj),
                             g_gizmo_op, g_gizmo_mode,
                             glm::value_ptr(model),
                             delta))
    {
        // Вращение у ImGuizmo идёт против курсора — применяем инверсию кадрового приращения.
        if (g_gizmo_op == ImGuizmo::ROTATE)
            model = glm::inverse(glm::make_mat4(delta)) * model_before;

        SetTransformCmd* cmd = new SetTransformCmd{};
        cmd->entity = selected;
        std::memcpy(cmd->matrix, glm::value_ptr(model), sizeof(cmd->matrix));
        ctx->GetInputManager()->PushCommand(CommandId::SetTransform, cmd);
    }
}
