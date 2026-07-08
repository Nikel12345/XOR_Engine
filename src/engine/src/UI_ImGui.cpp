#include "PCH.h"
#include "UI_ImGui.h"
#include "imgui_internal.h"   // DockBuilder* — первичная раскладка доков (см. SetupDockspace)
#include "EngineContext.h"
#include "InputManager.h"
#include "MaterialParams.h"   // раскладки факторов: разбор params по полям в инспекторе материала
#include "MaterialParamsRegistry.h"   // реестр типов params для дропдауна Kind
#include "ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>            // memcpy матрицы в payload команды
#include <algorithm>          // std::max / std::sort
#include <vector>             // списки имён для комбобоксов/плиток
#include <mutex>              // потокобезопасный приём пути из файл-диалога
#include <atomic>
#include <SDL3/SDL_dialog.h>  // нативный SDL_ShowOpenFileDialog

namespace {
    // ---- Выделение редактора: что сейчас показывает Inspector ----
    // Раньше выбор был только Entity (g_selected). Теперь общий: клик в Hierarchy или по
    // плитке ассета кладёт сюда «вид + идентификатор», а Inspector по нему решает, что рисовать.
    // Свет НЕ отдельный вид: он такая же сущность (Entity). Виды — только «сущность vs ресурс».
    // Shader = graphics sp, Compute = compute sp (2 типа, отдельные вкладки-фильтры).
    enum class SelKind { None, Entity, Camera, Material, Texture, Model, Shader, Compute };
    struct Selection {
        SelKind kind = SelKind::None;
        Entity  entity = static_cast<Entity>(-1);  // для Entity (включая свет)
        int     index = -1;                        // для Camera (порядковый)
        std::string name;                          // для Material / Texture / Model
    };
    Selection g_sel;

    constexpr Entity GIZMO_NONE = static_cast<Entity>(-1);
    // Полупрозрачный фон панелей (пустые места не глухо-чёрные, сквозь чуть видно сцену).
    constexpr float kPanelBgAlpha = 0.0f;

    // Общий тумблер показа служебных ассетов (имена на "_", напр. "_NoTextureDummy"). Делят
    // браузер ассетов и дропдауны текстур в инспекторе — одна галочка на всё.
    bool g_show_internal = false;
    bool IsInternalName(const std::string& n) { return !n.empty() && n[0] == '_'; }

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

    // ChannelConvention (enum) → строка для дропдауна конвенции каналов текстуры.
    const char* ConvName(ChannelConvention c)
    {
        switch (c) {
        case ChannelConvention::AsIs:              return "AsIs";
        case ChannelConvention::SmoothnessInGreen: return "SmoothnessInGreen";
        case ChannelConvention::DepthInAlpha:      return "DepthInAlpha";
        default:                                   return "AsIs";
        }
    }

    // AnchorShift (enum пивота модели) → строка для дропдауна.
    const char* AnchorName(AnchorShift a)
    {
        switch (a) {
        case AnchorShift::Keep:   return "Keep";
        case AnchorShift::Center: return "Center";
        case AnchorShift::LBB:    return "LBB";
        case AnchorShift::RBB:    return "RBB";
        case AnchorShift::LTB:    return "LTB";
        case AnchorShift::RTB:    return "RTB";
        case AnchorShift::LBF:    return "LBF";
        case AnchorShift::RBF:    return "RBF";
        case AnchorShift::LTF:    return "LTF";
        case AnchorShift::RTF:    return "RTF";
        default:                  return "Keep";
        }
    }

    // ================= Inspector-блоки (перенос существующих виджетов как есть) =================

    // Пред-объявления: InspectEntity дёргает свет-редакторы, определённые ниже.
    void InspectSpotLight(Positions&, size_t, SpotLightComponent&);
    void InspectSphereLight(Positions&, size_t, SphereLightComponent&);
    void InspectDirectLight(DirectLightComponent&);

    // Единый инспектор сущности: свет — такая же сущность, отдельного «типа выбора» нет. Показываем
    // то, что есть по компонентам: удаление (всегда) + трансформ (если Positions) + коллайдеры + свет.
    void InspectEntity(EngineContext* ctx, ObjectManager* om, SceneData* scene, Entity e)
    {
        ImGui::Text("Entity %u", static_cast<unsigned>(e));

        // Удаление — всегда (в т.ч. у directional-света без трансформа).
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
        const bool del = ImGui::Button("Delete", ImVec2(120.0f, 0.0f));
        ImGui::PopStyleColor(3);
        if (del) {
            ctx->GetInputManager()->PushCommand(CommandId::DeleteEntity,
                reinterpret_cast<const void*>(static_cast<uintptr_t>(e)));
            g_sel = Selection{};
            return;
        }

        // Трансформ — только если есть Positions.
        if (om->Has<Positions>(scene, e)) {
            SoAElement<Positions> el = om->GetComponent<Positions>(scene, e);
            Positions& P = el.container(); size_t i = el.i();
            float pos[3] = { P.w[i], P.d[i], P.h[i] };
            if (ImGui::DragFloat3("Offset", pos, 0.05f)) { P.w[i] = pos[0]; P.d[i] = pos[1]; P.h[i] = pos[2]; }
        }

        // Debug-рамки коллайдеров (дети): галочка visible → HideEntity.
        auto kids_it = scene->children.find(e);
        if (kids_it != scene->children.end() && !kids_it->second.empty()) {
            ImGui::SeparatorText("Debug colliders");
            for (Entity c : kids_it->second) {
                if (!om->Has<DrawComponent>(scene, c)) continue;
                bool visible = om->GetComponent<DrawComponent>(scene, c).visible;
                char clabel[40]; snprintf(clabel, sizeof(clabel), "visible (collider %u)", static_cast<unsigned>(c));
                if (ImGui::Checkbox(clabel, &visible)) {
                    const uintptr_t packed = static_cast<uintptr_t>(c)
                        | (visible ? (static_cast<uintptr_t>(1) << 32) : static_cast<uintptr_t>(0));
                    ctx->GetInputManager()->PushCommand(CommandId::HideEntity, reinterpret_cast<const void*>(packed));
                }
            }
        }

        // Свет — по наличию компонента (тип выбора не нужен).
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
    }

    void InspectMaterial(EngineContext* ctx, const std::string& matName, Material* mat)
    {
        if (!mat) { ImGui::TextUnformatted("Material not found."); return; }

        ShaderManager* sm = ctx->GetShaderManager();
        InputManager*  im = ctx->GetInputManager();

        // ---- Имя + переименование: галочка кликабельна ТОЛЬКО когда имя изменено, непусто и свободно.
        //      Переименование = ре-кей в словаре (delete+create). Ссылки по старому имени (энтити) после
        //      этого не резолвятся — переименовывай до назначения материала. ----
        static char nameBuf[128] = "";
        static std::string nameFor = "\x01";                 // сентинел → синк буфера на смену выбора
        if (matName != nameFor) { nameFor = matName; std::snprintf(nameBuf, sizeof nameBuf, "%s", matName.c_str()); }
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::SameLine();
        const bool nameChanged = nameBuf[0] && (matName != nameBuf)
            && !ctx->GetMaterialManager()->GetMaterials().count(nameBuf);
        {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float  sz = ImGui::GetFrameHeight();
            ImGui::BeginDisabled(!nameChanged);
            const bool apply = ImGui::Button("##rename", ImVec2(sz, sz));
            ImGui::EndDisabled();
            ImDrawList* dl = ImGui::GetWindowDrawList();      // галочку рисуем сами (в шрифте ImGui её нет)
            const ImU32 col = ImGui::GetColorU32(nameChanged ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            dl->AddLine({ p0.x + sz*0.24f, p0.y + sz*0.52f }, { p0.x + sz*0.42f, p0.y + sz*0.70f }, col, 2.0f);
            dl->AddLine({ p0.x + sz*0.42f, p0.y + sz*0.70f }, { p0.x + sz*0.78f, p0.y + sz*0.30f }, col, 2.0f);
            if (apply) {
                im->PushCommand(CommandId::RenameMaterial, new RenameMaterialCmd{ matName, nameBuf });
                g_sel.name = nameBuf;                          // выбор следует за переименованием
            }
        }

        // ================= Params (сверху) =================
        // Смена ТИПА params (kind) = SetMaterialParams<T> с дефолт-блобом (IN-PLACE, без ребилда:
        // RenderManager читает params->data()/size() живо; kind — фиксированный enum из реестра).
        ImGui::SeparatorText("Params");
        const auto& kinds = MaterialParamsRegistry::Get().All();
        const MaterialParamsTypeDesc* cur = nullptr;
        for (auto& d : kinds) if (d.kind == mat->params_kind) { cur = &d; break; }
        if (ImGui::BeginCombo("Kind", cur ? cur->label.c_str() : "?")) {
            for (auto& d : kinds) {
                bool is_cur = (d.kind == mat->params_kind);
                if (ImGui::Selectable(d.label.c_str(), is_cur) && !is_cur) d.applyDefault(ctx, mat);
                if (is_cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (mat->params.empty()) ImGui::TextDisabled("(no params)");
        else if (cur && cur->edit) cur->edit(mat);   // тип сам рисует свои поля (registry-driven)
        else {
            float* f = reinterpret_cast<float*>(mat->params.data());   // фолбэк: сырые float
            const size_t n = mat->params.size() / sizeof(float);
            for (size_t k = 0; k < n; ++k) {
                char l[16]; snprintf(l, sizeof(l), "p%u", static_cast<unsigned>(k));
                ImGui::SliderFloat(l, &f[k], 0.0f, 1.0f);
            }
        }

        // ================= Шейдеры + слоты (снизу) =================
        // Материал = набор sp (проходов). Слоты диктует required_slots КАЖДОГО sp, но текстура берётся
        // из ОБЩЕЙ material->textures[role] (роль шарится между sp): правка под одним sp видна под другим.
        ImGui::SeparatorText("Shaders");

        std::vector<std::string> texNames;                   // значения комбобокса текстур — по алфавиту
        for (auto& [n, h] : ctx->GetTextureManager()->GetTextureHandles())
            if (g_show_internal || !IsInternalName(n)) texNames.push_back(n);
        std::sort(texNames.begin(), texNames.end());

        for (size_t si = 0; si < mat->shader_programs.size(); ++si) {
            const std::string spName = mat->shader_programs[si];
            ImGui::PushID(static_cast<int>(si));

            if (ImGui::SmallButton("x"))                       // убрать этот sp
                im->PushCommand(CommandId::RemoveMaterialShader, new MaterialShaderCmd{ matName, spName });
            ImGui::SameLine();
            ImGui::TextUnformatted(spName.c_str());

            // Слоты этого sp; значение — из общей карты по роли (правка отражается во всех sp с этой ролью).
            if (ShaderProgram* sp = sm->GetShaderProgram(spName))
                for (TextureSlotRole role : sp->required_slots) {
                    auto it = mat->textures.find(role);
                    const std::string current = (it != mat->textures.end()) ? it->second : std::string();
                    ImGui::PushID(static_cast<int>(role));
                    if (ImGui::BeginCombo(RoleName(role), current.c_str())) {   // RoleName: enum слота → строка
                        for (const std::string& tn : texNames) {
                            bool is_cur = (tn == current);
                            if (ImGui::Selectable(tn.c_str(), is_cur) && !is_cur)
                                im->PushCommand(CommandId::SetMaterialTexture,
                                    new SetMaterialTextureCmd{ matName, static_cast<uint32_t>(role), tn });
                            if (is_cur) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
            ImGui::PopID();
            ImGui::Separator();
        }

        // Добавить sp (перечень graphics sp, ещё не добавленных материалу).
        if (ImGui::BeginCombo("+ Shader", "(add)")) {
            for (auto& [spn, spp] : sm->GetShaderPrograms()) {
                bool present = false;
                for (auto& s : mat->shader_programs) if (s == spn) { present = true; break; }
                if (present) continue;
                if (ImGui::Selectable(spn.c_str()))
                    im->PushCommand(CommandId::AddMaterialShader, new MaterialShaderCmd{ matName, spn });
            }
            ImGui::EndCombo();
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

    // --- Приём пути из нативного файл-диалога. Колбэк SDL может прийти из ДРУГОГО потока, поэтому
    //     кладём через мьютекс+atomic, а UI забирает у себя на кадре. g_pick_target помечает, КАКОЕ
    //     поле заполнять (у формы модели два пути) — активная форма забирает только «своё». ---
    enum class PickTarget { None, TexPath, ModelVert, ModelIndex };
    PickTarget        g_pick_target = PickTarget::None;
    std::mutex        g_pick_mtx;
    std::string       g_picked_path;
    std::atomic<bool> g_picked_ready{ false };

    void SDLCALL OnFilePicked(void*, const char* const* filelist, int)
    {
        if (filelist && filelist[0]) {                 // пусто = отмена, nullptr = ошибка
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            g_picked_path = filelist[0];
            g_picked_ready.store(true, std::memory_order_release);
        }
    }

    // Открыть нативный диалог, пометив целевое поле (SDL требует main-поток; результат — потокобезопасно).
    void OpenFileDialog(PickTarget target, const SDL_DialogFileFilter* filters, int nfilters)
    {
        g_pick_target = target;
        SDL_ShowOpenFileDialog(OnFilePicked, nullptr, nullptr, filters, nfilters, nullptr, false);
    }

    // Форма создания/редактирования текстуры (одна и та же — см. upsert delete+create). Ничего не
    // происходит по-символьно: правки копятся в буферах, действие только по кнопке.
    void TextureEditor(EngineContext* ctx)
    {
        static char        nameBuf[128] = "";
        static char        pathBuf[512] = "";
        static std::string atlasSel;
        static ChannelConvention convSel = ChannelConvention::AsIs;
        static std::string syncedFor = "\x01";   // сентинел ≠ любому имени → синк на первый заход

        // Имя подтягиваем из выбора (клик по плитке = редактировать её; "New" = пустое имя).
        // atlas/path НЕ сбрасываем — удобно заливать серию текстур в тот же атлас.
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            if (!g_sel.name.empty()) {
                // Ресурс самоописываем: тянем атлас/путь прямо из хэндла (см. TextureHandle).
                if (TextureHandle* h = ctx->GetTextureManager()->GetTextureHandle(g_sel.name)) {
                    atlasSel = h->atlas_name;
                    std::snprintf(pathBuf, sizeof pathBuf, "%s", h->source_path.c_str());
                    convSel = h->conv;
                }
            }
            else pathBuf[0] = '\0';   // "New" — путь чистим (атлас оставляем для серии)
        }

        // Забрать путь, выбранный в диалоге (только если он для нашего поля).
        if (g_pick_target == PickTarget::TexPath && g_picked_ready.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            std::snprintf(pathBuf, sizeof pathBuf, "%s", g_picked_path.c_str());
            g_pick_target = PickTarget::None;
        }

        ImGui::TextDisabled("Texture (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);

        // Атлас — дропдаун существующих (общий фильтр служебных с браузером).
        if (ImGui::BeginCombo("Atlas", atlasSel.empty() ? "(select)" : atlasSel.c_str())) {
            for (auto& [an, a] : ctx->GetTextureManager()->GetAtlases()) {
                if (!g_show_internal && IsInternalName(an)) continue;
                bool sel = (an == atlasSel);
                if (ImGui::Selectable(an.c_str(), sel)) atlasSel = an;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            static const SDL_DialogFileFilter filters[] = {
                { "Images", "png;jpg;jpeg;bmp;tga" }, { "All files", "*" }
            };
            OpenFileDialog(PickTarget::TexPath, filters, 2);
        }

        // Конвенция каналов исходника (enum → switch, см. ConvName).
        static const ChannelConvention kConvs[] = {
            ChannelConvention::AsIs, ChannelConvention::SmoothnessInGreen, ChannelConvention::DepthInAlpha
        };
        if (ImGui::BeginCombo("Channels", ConvName(convSel))) {
            for (ChannelConvention c : kConvs) {
                bool sel = (c == convSel);
                if (ImGui::Selectable(ConvName(c), sel)) convSel = c;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const bool ready = nameBuf[0] && !atlasSel.empty() && pathBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Create / Update", ImVec2(160, 0))) {
            // old_name = выбранная текстура: если имя изменили — переименование (старую снять в команде).
            ctx->GetInputManager()->PushCommand(CommandId::UpsertTexture,
                new UpsertTextureCmd{ nameBuf, atlasSel, pathBuf, static_cast<uint32_t>(convSel), g_sel.name });
            g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = nameBuf;   // выбор следует за именем
        }
        ImGui::EndDisabled();

        // Удаление существующей текстуры (материалы по её имени → dummy на пересборке).
        if (!g_sel.name.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
            const bool del = ImGui::Button("Delete");
            ImGui::PopStyleColor(2);
            if (del) {
                ctx->GetInputManager()->PushCommand(CommandId::DeleteTexture, new DeleteTextureCmd{ g_sel.name });
                g_sel = Selection{};
            }
        }
    }

    // Форма создания/редактирования модели (аналог TextureEditor). Upsert = перезагрузка in-place
    // (ModelManager::LoadModelFromFile). Процедурные модели имеют пустые пути → форма пуста, а кнопка
    // (нужны оба пути) не активна — их создание только в коде.
    void ModelEditor(EngineContext* ctx)
    {
        static char        nameBuf[128] = "";
        static char        modelBuf[512] = "";
        static char        indexBuf[512] = "";
        static AnchorShift anchorSel = AnchorShift::Keep;
        static std::string syncedFor = "\x01";

        if (g_sel.name != syncedFor) {                        // синк из выбранной модели (self-describing)
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            if (!g_sel.name.empty()) {
                auto& models = ctx->GetModelManager()->GetModels();
                auto it = models.find(g_sel.name);
                if (it != models.end()) {
                    std::snprintf(modelBuf, sizeof modelBuf, "%s", it->second->model_path.c_str());
                    std::snprintf(indexBuf, sizeof indexBuf, "%s", it->second->index_path.c_str());
                    anchorSel = it->second->anchor;
                }
            }
            else { modelBuf[0] = '\0'; indexBuf[0] = '\0'; }
        }

        // Забрать путь из диалога в нужное из двух полей.
        if ((g_pick_target == PickTarget::ModelVert || g_pick_target == PickTarget::ModelIndex)
            && g_picked_ready.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            char* dst = (g_pick_target == PickTarget::ModelVert) ? modelBuf : indexBuf;
            std::snprintf(dst, 512, "%s", g_picked_path.c_str());
            g_pick_target = PickTarget::None;
        }

        static const SDL_DialogFileFilter filters[] = { { "Model (.bin)", "bin" }, { "All files", "*" } };

        ImGui::TextDisabled("Model (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);

        ImGui::InputText("Vertices", modelBuf, sizeof modelBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##v")) OpenFileDialog(PickTarget::ModelVert, filters, 2);

        ImGui::InputText("Indices", indexBuf, sizeof indexBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##i")) OpenFileDialog(PickTarget::ModelIndex, filters, 2);

        // Пивот (anchor) — enum → дропдаун (см. AnchorName).
        static const AnchorShift kAnchors[] = {
            AnchorShift::Keep, AnchorShift::Center, AnchorShift::LBB, AnchorShift::RBB,
            AnchorShift::LTB, AnchorShift::RTB, AnchorShift::LBF, AnchorShift::RBF,
            AnchorShift::LTF, AnchorShift::RTF
        };
        if (ImGui::BeginCombo("Anchor", AnchorName(anchorSel))) {
            for (AnchorShift a : kAnchors) {
                bool sel = (a == anchorSel);
                if (ImGui::Selectable(AnchorName(a), sel)) anchorSel = a;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const bool ready = nameBuf[0] && modelBuf[0] && indexBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Create / Update", ImVec2(160, 0)))
            ctx->GetInputManager()->PushCommand(CommandId::UpsertModel,
                new UpsertModelCmd{ nameBuf, modelBuf, indexBuf, static_cast<uint32_t>(anchorSel) });
        ImGui::EndDisabled();
    }

    // Инспектор graphics-sp: параметры spd (живут В sp) как галочки/списки. Взаимоисключающие
    // (cull/fill/primitive) — списки, булевы — галочки. Push/dispatch в UI НЕ идут (это код).
    // Правка — in-place в sp->spd; при изменении шлём команду пересоздать пайплайн (строится из spd).
    void ShaderInspector(EngineContext* ctx, const std::string& spName)
    {
        ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(spName);
        if (!sp) { ImGui::TextUnformatted("Shader not found."); return; }
        ImGui::Text("Shader: %s", spName.c_str());

        // Удаление sp: пайплайн в отложенное удаление, шейдеры релизятся по refcount, материалы → fallback.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
        const bool del = ImGui::Button("Delete shader");
        ImGui::PopStyleColor(2);
        if (del) {
            ctx->GetInputManager()->PushCommand(CommandId::DeleteShader, new RebuildShaderPipelineCmd{ spName });
            g_sel = Selection{};
            return;
        }

        ImGui::SeparatorText("Pipeline state (spd)");

        ShaderProgramDescription& d = sp->spd;
        bool changed = false;

        // Взаимоисключающие enum'ы — списки.
        { const char* names[] = { "None", "Front", "Back" }; int v = static_cast<int>(d.cull_mode);
          if (ImGui::Combo("Cull mode", &v, names, 3)) { d.cull_mode = static_cast<SDL_GPUCullMode>(v); changed = true; } }
        { const char* names[] = { "Fill", "Wireframe" }; int v = static_cast<int>(d.fill_mode);
          if (ImGui::Combo("Fill mode", &v, names, 2)) { d.fill_mode = static_cast<SDL_GPUFillMode>(v); changed = true; } }
        { const char* names[] = { "TriangleList", "TriangleStrip", "LineList", "LineStrip", "PointList" };
          int v = static_cast<int>(d.primitive_type);
          if (ImGui::Combo("Primitive", &v, names, 5)) { d.primitive_type = static_cast<SDL_GPUPrimitiveType>(v); changed = true; } }

        // Булевы — галочки.
        changed |= ImGui::Checkbox("Depth test",   &d.depth_test);
        changed |= ImGui::Checkbox("Depth write",  &d.depth_write);
        changed |= ImGui::Checkbox("Stencil test", &d.stencil_test);
        changed |= ImGui::Checkbox("Color blend",  &d.color_blend);

        // Depth bias — галочка + поля (показываем поля только когда включён).
        changed |= ImGui::Checkbox("Depth bias", &d.rasterizer_bias.enable_depth_bias);
        if (d.rasterizer_bias.enable_depth_bias) {
            changed |= ImGui::DragFloat("Bias constant", &d.rasterizer_bias.depth_bias_constant_factor, 0.05f);
            changed |= ImGui::DragFloat("Bias slope",    &d.rasterizer_bias.depth_bias_slope_factor, 0.05f);
            changed |= ImGui::DragFloat("Bias clamp",    &d.rasterizer_bias.depth_bias_clamp, 0.05f);
        }

        if (changed)   // spd уже поправлен in-place; команда пересоздаёт пайплайн + пересобирает батчи
            ctx->GetInputManager()->PushCommand(CommandId::RebuildShaderPipeline,
                new RebuildShaderPipelineCmd{ spName });
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

void UI_ImGui::DrawInspector(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Inspector");

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();

    // Режим гизмо — только если у выбранной сущности есть Positions (иначе двигать нечего).
    if (g_sel.kind == SelKind::Entity && scene && om->Has<Positions>(scene, g_sel.entity)) {
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
    }

    switch (g_sel.kind)
    {
    case SelKind::Entity:
        if (scene) InspectEntity(ctx, om, scene, g_sel.entity);   // трансформ/удаление/коллайдеры + свет по компонентам
        break;

    case SelKind::Camera:
        InspectCamera(ctx->GetCameraManager()->GetActiveCamera());
        break;

    case SelKind::Material:
        InspectMaterial(ctx, g_sel.name, ctx->GetMaterialManager()->GetMaterial(g_sel.name));
        break;

    case SelKind::Texture:
        TextureEditor(ctx);
        break;

    case SelKind::Model:
        ModelEditor(ctx);
        break;

    case SelKind::Shader:
        ShaderInspector(ctx, g_sel.name);
        break;

    case SelKind::Compute:
        ImGui::Text("Compute: %s", g_sel.name.c_str());
        ImGui::TextDisabled("(no editable pipeline state; push/dispatch are code)");
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

    // Фильтр служебных ассетов: общий флаг (см. g_show_internal) — та же галочка действует и на
    // дропдауны текстур в инспекторе.
    ImGui::Checkbox("Show internal (_)", &g_show_internal);
    ImGui::Separator();

    const float tile = 64.0f, pad = 8.0f;

    if (ImGui::BeginTabBar("AssetTabs"))
    {
        // Общая раскладка плиток: переносим ряд, когда следующая не влезает по ширине.
        auto tiles = [&](SelKind kind, bool withNew, auto&& onNew, auto&& for_each_name)
        {
            float avail = ImGui::GetContentRegionAvail().x;
            int per_row = std::max(1, static_cast<int>(avail / (tile + pad)));
            int col = 0;
            auto step = [&]{ if (++col % per_row != 0) ImGui::SameLine(); };

            if (withNew) {
                // Плитка-«плюс» того же размера, что превью. Действие — за onNew (у текстур форма,
                // у материалов сразу команда создания).
                ImGui::BeginGroup();
                ImGui::PushID("##new");
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                if (ImGui::Button("##new", ImVec2(tile, tile))) onNew();
                // «+» рисуем сами почти во весь квадрат (кнопочный текст "+" был бы крохотным).
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float cx = p0.x + tile * 0.5f, cy = p0.y + tile * 0.5f;
                const float arm = tile * 0.35f, th = tile * 0.076f;   // толщина на ~5% меньше
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddRectFilled(ImVec2(cx - th, cy - arm), ImVec2(cx + th, cy + arm), col);   // вертикаль
                dl->AddRectFilled(ImVec2(cx - arm, cy - th), ImVec2(cx + arm, cy + th), col);   // горизонталь
                ImGui::TextUnformatted("New");
                ImGui::PopID();
                ImGui::EndGroup();
                step();
            }

            for_each_name([&](const std::string& name)
            {
                if (!g_show_internal && IsInternalName(name)) return;   // фильтр служебных
                bool selected = (g_sel.kind == kind && g_sel.name == name);
                if (AssetTile(name.c_str(), selected, tile)) {
                    if (selected) g_sel = Selection{};                                 // повторный клик — снять
                    else { g_sel = Selection{}; g_sel.kind = kind; g_sel.name = name; }
                }
                step();
            });
        };

        if (ImGui::BeginTabItem("Materials")) {
            tiles(SelKind::Material, true,
                [&]{
                    // Свободное имя считаем в UI → сразу ставим выбор на создаваемый материал.
                    auto& mats = ctx->GetMaterialManager()->GetMaterials();
                    std::string nm = "material";
                    for (int i = 1; mats.count(nm); ++i) nm = "material_" + std::to_string(i);
                    g_sel = Selection{}; g_sel.kind = SelKind::Material; g_sel.name = nm;
                    ctx->GetInputManager()->PushCommand(CommandId::CreateMaterial, new CreateMaterialCmd{ nm });
                },
                [&](auto&& emit) { for (auto& [name, mat] : ctx->GetMaterialManager()->GetMaterials()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            tiles(SelKind::Texture, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = ""; },   // + = форма новой текстуры
                [&](auto&& emit) { for (auto& [name, h] : ctx->GetTextureManager()->GetTextureHandles()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Models")) {
            tiles(SelKind::Model, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Model; g_sel.name = ""; },   // + = форма новой модели
                [&](auto&& emit) { for (auto& [name, m] : ctx->GetModelManager()->GetModels()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shaders")) {                                        // graphics sp (создание — код)
            tiles(SelKind::Shader, false, []{},
                [&](auto&& emit) { for (auto& [name, sp] : ctx->GetShaderManager()->GetShaderPrograms()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compute")) {                                        // compute sp
            tiles(SelKind::Compute, false, []{},
                [&](auto&& emit) { for (auto& csp : ctx->GetShaderManager()->GetComputeShaderPrograms()) emit(csp->debug_name); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void UI_ImGui::DrawGizmo(EngineContext* ctx)
{
    // Гизмо — для выбранной сущности (свет = тоже сущность). Для материала/текстуры/… нет.
    if (g_sel.kind != SelKind::Entity) return;
    Entity selected = g_sel.entity;

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    // Нет трансформа (directional-свет без Positions либо удалённая сущность) → просто без гизмо.
    if (!om->Has<Positions>(scene, selected)) return;

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
