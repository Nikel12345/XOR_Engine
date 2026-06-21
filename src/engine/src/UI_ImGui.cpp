#include "PCH.h"
#include "UI_ImGui.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "MaterialParams.h"   // раскладки факторов: разбор params по полям в панели Materials
#include "ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>            // memcpy матрицы в payload команды

namespace {
    // «Выделение» для гизмо — это просто текущая сущность. Пикинга по сцене нет:
    // выбор делается кликом в списке объектов (radio «Gizmo»). Сентинел = нет выбора.
    constexpr Entity GIZMO_NONE = static_cast<Entity>(-1);
    Entity g_selected = GIZMO_NONE;

    // Текущий режим стрелок. Переключается радиокнопками в панели Objects.
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
}

void UI_ImGui::Iterate(EngineContext* ctx)
{
    // Должен идти раз за кадр после ImGui::NewFrame (его зовёт Engine::BeginImGuiFrame).
    ImGuizmo::BeginFrame();

    ImGui::Begin("Debug");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Separator();

    DrawCameraPanel(ctx->GetCameraManager());
    ImGui::Separator();
    DrawObjectsPanel(ctx);
    ImGui::Separator();
    DrawMaterialsPanel(ctx);
    ImGui::Separator();
    DrawLightsPanel(ctx->GetObjectManager());
    ImGui::End();

    // Гизмо рисуем ПОСЛЕ End() — оно живёт не в окне-панели, а поверх сцены
    // (собственный draw-list), иначе бы клипалось границами «Debug».
    DrawGizmo(ctx);
}

void UI_ImGui::DrawCameraPanel(CameraManager* cameraManager)
{
    Camera* cam = cameraManager->GetActiveCamera();
    if (!cam) return;

    if (ImGui::CollapsingHeader("Camera"))
    {
        glm::vec3 pos = cam->GetPosition();
        glm::vec3 tgt = cam->GetTarget();
        if (ImGui::DragFloat3("Cam Position", &pos.x, 0.05f))
            cam->SetPosition(pos);
        if (ImGui::DragFloat3("Cam Target", &tgt.x, 0.05f))
            cam->SetView(pos, tgt, glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

void UI_ImGui::DrawObjectsPanel(EngineContext* ctx)
{
    if (!ImGui::CollapsingHeader("Objects")) return;

    ObjectManager* objectManager = ctx->GetObjectManager();
    SceneData* scene = objectManager->GetActiveScene();

    if (ImGui::TreeNode("Mesh Objects"))
    {
        std::vector<Entity> to_delete;
        std::vector<std::pair<Entity, bool>> to_set_visible;

        // Режим стрелок трансформации + сброс выбора. Сам выбор сущности — radio
        // «Gizmo» внутри узла каждого объекта ниже (пикинга по сцене пока нет).
        ImGui::TextUnformatted("Gizmo:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Move",   g_gizmo_op == ImGuizmo::TRANSLATE)) g_gizmo_op = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", g_gizmo_op == ImGuizmo::ROTATE))    g_gizmo_op = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale",  g_gizmo_op == ImGuizmo::SCALE))     g_gizmo_op = ImGuizmo::SCALE;
        ImGui::SameLine();
        // Равномерный масштаб (все оси разом) отдельным режимом — у ImGuizmo это
        // SCALEU: вместо трёх осевых ручек тянешь одну, объект масштабируется целиком.
        if (ImGui::RadioButton("Uniform", g_gizmo_op == ImGuizmo::SCALEU))    g_gizmo_op = ImGuizmo::SCALEU;
        ImGui::SameLine();
        if (ImGui::SmallButton("Deselect")) g_selected = GIZMO_NONE;

        objectManager->ForEach<Positions, MaterialComponent, ModelComponent>(scene,
            [&](Entity e, SoAElement<Positions> pos_el, MaterialComponent&, ModelComponent&)
        {
            // Вспомогательная визуализация (debug-рамки коллайдеров и т.п.) помечена
            // движковым тегом EditorHiddenComponent — в список реальных объектов не выводим.
            if (objectManager->Has<EditorHiddenComponent>(scene, e)) return;

            Positions& P = pos_el.container();
            size_t i = pos_el.i();

            char label[32];
            snprintf(label, sizeof(label), "Entity %u", static_cast<unsigned>(e));

            if (ImGui::TreeNode(label))
            {
                // Выбор этой сущности для гизмо (radio = текущее выделение).
                if (ImGui::RadioButton("Gizmo", g_selected == e)) g_selected = e;

                float pos[3] = { P.w[i], P.d[i], P.h[i] };
                if (ImGui::DragFloat3("Offset", pos, 0.05f))
                {
                    P.w[i] = pos[0];
                    P.d[i] = pos[1];
                    P.h[i] = pos[2];
                }

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.10f, 0.10f, 1.0f));

                if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) to_delete.push_back(e);

                ImGui::PopStyleColor(3);

                // Дети владельца — debug-рамки коллайдеров (DrawComponent + ParentComponent,
                // помечены EditorHiddenComponent, потому в основной список не попадают).
                // Галочка visible каждого: снятие прячет рамку из рендера, НЕ удаляя энтити.
                // UI сам ECS не мутирует — кладёт команду HideEntity в очередь, исполнит
                // sim-поток (флаг visible там лишь для отображения текущего состояния).
                auto kids_it = scene->children.find(e);
                if (kids_it != scene->children.end() && !kids_it->second.empty())
                {
                    ImGui::SeparatorText("Debug colliders");
                    for (Entity c : kids_it->second)
                    {
                        if (!objectManager->Has<DrawComponent>(scene, c)) continue;
                        bool visible = objectManager->GetComponent<DrawComponent>(scene, c).visible;

                        char clabel[40];
                        snprintf(clabel, sizeof(clabel), "visible (collider %u)",
                                 static_cast<unsigned>(c));
                        if (ImGui::Checkbox(clabel, &visible))
                            to_set_visible.emplace_back(c, visible);
                    }
                }

                ImGui::TreePop();
            }
        });

        // UI ничего не мутирует сам: кладёт команду "удалить этот entity"
        // в очередь, sim-поток исполнит её в MainIterate. Данных-структуры нет —
        // entity пакуем прямо в указатель (никакого копирования/владения).
        for (Entity e : to_delete)
            ctx->GetInputManager()->PushCommand(CommandId::DeleteEntity,
                reinterpret_cast<const void*>(static_cast<uintptr_t>(e)));

        // Те же правила времени жизни, что у Delete: данные не копируются — Entity и
        // флаг visible пакуем прямо в указатель (Entity в младших 32 битах, visible — бит 32).
        for (const auto& [c, visible] : to_set_visible)
        {
            const uintptr_t packed = static_cast<uintptr_t>(c)
                | (visible ? (static_cast<uintptr_t>(1) << 32) : static_cast<uintptr_t>(0));
            ctx->GetInputManager()->PushCommand(CommandId::HideEntity,
                reinterpret_cast<const void*>(packed));
        }

        ImGui::TreePop();
    }
}

void UI_ImGui::DrawGizmo(EngineContext* ctx)
{
    if (g_selected == GIZMO_NONE) return;

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    // Сущность могли удалить через очередь команд — снимаем выбор и выходим.
    if (!om->Has<Positions>(scene, g_selected)) { g_selected = GIZMO_NONE; return; }

    Camera* cam = ctx->GetCameraManager()->GetActiveCamera();
    if (!cam) return;
    // GetView/GetProj отдают по значению — кладём в локали, чтобы взять value_ptr.
    glm::mat4 view = cam->GetView();
    glm::mat4 proj = cam->GetProj();

    SoAElement<Positions> el = om->GetComponent<Positions>(scene, g_selected);
    Positions& P = el.container();
    const size_t i = el.i();

    glm::mat4 model = ReadPositionsMatrix(P, i);
    const glm::mat4 model_before = model;   // матрица ДО манипуляции (для разворота поворота)

    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetOrthographic(false);
    // Фон-лист: стрелки поверх 3D-сцены, но под окнами ImGui.
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    float delta[16];   // приращение этого кадра (мировое), нужно для разворота вращения
    if (ImGuizmo::Manipulate(glm::value_ptr(view),
                             glm::value_ptr(proj),
                             g_gizmo_op, g_gizmo_mode,
                             glm::value_ptr(model),
                             delta))
    {
        // Вращение у ImGuizmo идёт против курсора. «Сменить знак» поворота = применить
        // к матрице ДО манипуляции ИНВЕРСИЮ кадрового приращения вместо него самого
        // (delta — чистый поворот в режиме ROTATE, инверсия = поворот в обратную сторону).
        // Move/Scale/Uniform оставляем как есть.
        if (g_gizmo_op == ImGuizmo::ROTATE)
            model = glm::inverse(glm::make_mat4(delta)) * model_before;

        // Тащат стрелку — шлём новую мировую матрицу в sim-поток (он пишет в Positions).
        // payload на куче: 16 float не лезут в указатель; функтор команды его удалит.
        SetTransformCmd* cmd = new SetTransformCmd{};
        cmd->entity = g_selected;
        std::memcpy(cmd->matrix, glm::value_ptr(model), sizeof(cmd->matrix));
        ctx->GetInputManager()->PushCommand(CommandId::SetTransform, cmd);
    }
}

void UI_ImGui::DrawMaterialsPanel(EngineContext* ctx)
{
    if (!ImGui::CollapsingHeader("Materials")) return;

    MaterialManager* mm = ctx->GetMaterialManager();
    if (!mm) return;

    for (auto& [name, mat] : mm->GetMaterials())
    {
        if (!mat || mat->params.empty()) continue;   // только материалы с факторами

        if (!ImGui::TreeNode(name.c_str())) continue;

        // Правка идёт ПРЯМО в material->params: батч держит на него указатель и пушит каждый
        // кадр, а ключ батча = идентичность материала → мутация НЕ вызывает пересборку дерева
        // (живой тюнинг без ребилда). Тег params_kind выбирает рукописный разбор по полям
        // (C++ рефлексии нет — подписи захардкожены здесь; тег лишь дискриминатор + доступ
        // к полям по имени через каст к раскладке из MaterialParams.h).
        switch (mat->params_kind)
        {
        case MaterialParamsKind::Opaque:
        {
            auto* p = reinterpret_cast<OpaqueMaterialParams*>(mat->params.data());
            ImGui::ColorEdit3("Base Color", p->baseColor);
            // По мере раскомментирования полей в OpaqueMaterialParams добавляй виджеты сюда:
            // ImGui::SliderFloat("Metallic",  &p->metallic,  0.0f, 1.0f);
            // ImGui::SliderFloat("Roughness", &p->roughness, 0.0f, 1.0f);
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
            // Неизвестная раскладка (None) — fallback на сырые float'ы.
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
        ImGui::TreePop();
    }
}

void UI_ImGui::DrawLightsPanel(ObjectManager* objectManager)
{
    if (!ImGui::CollapsingHeader("Lights")) return;
    SceneData* scene = objectManager->GetActiveScene();

    // ---------- Spot Lights ----------
    if (ImGui::TreeNode("Spot Lights"))
    {
        int index = 0;
        objectManager->ForEach<Positions, SpotLightComponent>(scene,
            [&index](SoAElement<Positions> pos_el, SpotLightComponent& light)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();

            char label[32];
            snprintf(label, sizeof(label), "Spot %d", index++);
            if (!ImGui::TreeNode(label)) return;

            auto& d = light.light_data;
            bool changed = false;

            // --- Положение / поворот ---
            ImGui::SeparatorText("Transform");
            float pos[3] = { P.w[i], P.d[i], P.h[i] };
            if (ImGui::DragFloat3("Position", pos, 0.05f))
            {
                P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2];
            }
            float dir[3] = { d.dir_x, d.dir_y, d.dir_z };
            if (ImGui::DragFloat3("Direction", dir, 0.01f, -1.0f, 1.0f))
            {
                d.dir_x = dir[0]; d.dir_y = dir[1]; d.dir_z = dir[2];
                changed = true;
            }

            // --- Конус ---
            ImGui::SeparatorText("Cone");
            // source_angle в радианах -> SliderAngle показывает градусы.
            // Ограничиваем < 90°, иначе tan() уходит в бесконечность в ResolveDistance().
            changed |= ImGui::SliderAngle("Angle", &d.source_angle, 1.0f, 89.0f);
            // усечение конуса
            changed |= ImGui::DragFloat("Source Radius", &d.source_radius, 0.01f, 0.0f, FLT_MAX);

            // --- Цвет ---
            ImGui::SeparatorText("Color");
            changed |= ImGui::ColorEdit3("RGB", &d.r); // r,g,b лежат подряд в памяти

            // --- Затухание / мощность ---
            ImGui::SeparatorText("Falloff");
            changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);
            changed |= ImGui::DragFloat("Attenuation", &d.attenuation, 0.05f, 0.0f, FLT_MAX);

            d.ResolveDistance(); // лениво пересчитает max_distance, если что-то менялось
            ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());

            if (changed) light.needsUpdate = true;
            ImGui::TreePop();
        });
        ImGui::TreePop();
    }

    // ---------- Sphere Lights ----------
    if (ImGui::TreeNode("Sphere Lights"))
    {
        int index = 0;
        objectManager->ForEach<Positions, SphereLightComponent>(scene,
            [&index](SoAElement<Positions> pos_el, SphereLightComponent& light)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();

            char label[32];
            snprintf(label, sizeof(label), "Sphere %d", index++);
            if (!ImGui::TreeNode(label)) return;

            auto& d = light.light_data;
            bool changed = false;

            // --- Положение ---
            ImGui::SeparatorText("Transform");
            float pos[3] = { P.w[i], P.d[i], P.h[i] };
            if (ImGui::DragFloat3("Position", pos, 0.05f))
            {
                P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2];
            }

            // --- Радиус ---
            ImGui::SeparatorText("Shape");
            changed |= ImGui::DragFloat("Radius", &d.source_radius, 0.01f, 0.0f, FLT_MAX);

            // --- Цвет ---
            ImGui::SeparatorText("Color");
            changed |= ImGui::ColorEdit3("RGB", &d.r);

            // --- Затухание / мощность ---
            ImGui::SeparatorText("Falloff");
            changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);
            changed |= ImGui::DragFloat("Attenuation", &d.attenuation, 0.05f, 0.0f, FLT_MAX);

            d.ResolveDistance();
            ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());

            if (changed) light.needsUpdate = true;
            ImGui::TreePop();
        });
        ImGui::TreePop();
    }

    // ---------- Directional Lights ----------
    if (ImGui::TreeNode("Directional Lights"))
    {
        int index = 0;
        objectManager->ForEach<DirectLightComponent>(scene,
            [&index](DirectLightComponent& light)
        {
            char label[32];
            snprintf(label, sizeof(label), "Directional %d", index++);
            if (!ImGui::TreeNode(label)) return;

            auto& d = light.light_data;
            bool changed = false;

            // --- Направление (нормализуется при заливке; позиции у directional нет) ---
            ImGui::SeparatorText("Direction");
            changed |= ImGui::DragFloat3("Dir", &d.dir_x, 0.01f, -1.0f, 1.0f);

            // --- Цвет / мощность ---
            ImGui::SeparatorText("Color");
            changed |= ImGui::ColorEdit3("RGB", &d.r);
            changed |= ImGui::DragFloat("Power", &d.power, 0.05f, 0.0f, FLT_MAX);

            // --- Статичные вложенные ortho-боксы (каскады) ---
            ImGui::SeparatorText("Shadow Cascades");
            changed |= ImGui::DragFloat3("Center", &d.center_x, 0.05f);
            changed |= ImGui::DragFloat("Half Extent (c0)", &d.half_extent, 0.1f, 0.01f, FLT_MAX);
            changed |= ImGui::DragFloat("Half Depth (c0)", &d.half_depth, 0.1f, 0.01f, FLT_MAX);
            changed |= ImGui::DragFloat("Cascade Ratio", &d.cascade_ratio, 0.05f, 1.0f, FLT_MAX);
            // Меняет число теневых камер → буферы пересчитываются. Степпер +/- с клампом
            // в [1, MAX_CASCADES] (все каскады всех светов делят 8-слойную карту).
            if (ImGui::InputInt("Cascade Count", &d.cascade_count)) {
                if (d.cascade_count < 1) d.cascade_count = 1;
                if (d.cascade_count > DirectLightComponent::DirectLightData::MAX_CASCADES)
                    d.cascade_count = DirectLightComponent::DirectLightData::MAX_CASCADES;
                changed = true;
            }

            // Per-cascade: латераль x глубина и мир-на-тексель (ширина / 1024).
            for (int c = 0; c < d.cascade_count; ++c) {
                float he = d.CascadeExtent(c);
                float dp = d.CascadeDepth(c);
                ImGui::Text("  c%d: %.1f x %.1f, depth %.1f, texel %.4f",
                    c, 2.0f * he, 2.0f * he, 2.0f * dp, (2.0f * he) / 1024.0f);
            }

            if (changed) light.needsUpdate = true;
            ImGui::TreePop();
        });
        ImGui::TreePop();
    }
}