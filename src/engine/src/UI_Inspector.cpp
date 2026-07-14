#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "imgui_internal.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "InputCommands.h"  // payload-структуры команд редактора
// EngineContext.h держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "ObjectManager.h"
#include "CameraManager.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ShaderManager.h"
#include "BatchBuilder.h"
#include "MaterialParams.h"          // раскладки факторов: разбор params по полям в инспекторе материала
#include "MaterialParamsRegistry.h"  // реестр типов params для дропдауна Kind
#include "ComponentSerializer.h"     // ComponentSpecRegistry — цикл по компонентам энтити
#include "UI_ComponentEditor.h"      // generic-редактор полей компонента по схеме
#include "RenderManager.h"           // PassManager + RenderPassStep — дропдаун прохода у sp
#include "ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>            // memcpy матрицы в payload команды
#include <mutex>              // потокобезопасный приём пути из файл-диалога
#include <atomic>

using namespace ShaderBase;   // VertexSemantic в редакторе pull вершинника
#include <SDL3/SDL_dialog.h>  // нативный SDL_ShowOpenFileDialog

using namespace ui;

// Правая панель целиком: диспетчер DrawInspector + все Inspect*-блоки/редакторы (внизу), плюс
// гизмо (DrawGizmo). Хелперы файл-локальны (анонимный namespace) — их зовёт только этот TU.

namespace {
    // Текущий режим стрелок. Переключается радиокнопками в Inspector, читается в DrawGizmo.
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

    // Маркер «ссылка по имени не резолвится» в конце строки: жёлтый (!) + тултип с причиной.
    // Икон-шрифта у ImGui нет — обычный цветной текст. Рендер при таких промахах не падает
    // (dummy/fallback на пересборке), маркер лишь делает промах видимым в инспекторе.
    void MissingRefMark(const char* tooltip)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(!)");
        ImGui::SetItemTooltip("%s", tooltip);
    }

    // ================= Inspector-блоки =================

    // ShaderInspector дёргает редакторы списков буферов/слот-ролей, определённые ниже (рядом с формами SD).
    void BufferListEditor(const char* label, std::vector<BufferDataName>& list, const std::vector<BufferDataName>& avail);
    void RoleListEditor(std::vector<TextureSlotRole>& list);

    // Пост-блоки компонентов после generic-полей: производные значения, которые схема не
    // описывает (вычисления, не поля), и контролы-через-команду (visible). Рукописный
    // остаток UI компонентов — всё остальное рисует DrawComponentFields по схеме.
    void ComponentExtraUI(EngineContext* ctx, Entity e, const std::string& name, Archetype& arch, size_t row)
    {
        if (name == "Draw") {
            // visible — ЧЕРЕЗ команду HideEntity (та же упаковка, что у рамок коллайдеров):
            // прямая запись флага не перестроила бы батчи, поэтому в схеме поле ui_hidden.
            bool visible = (*arch.get_array<DrawComponent>())[row].visible;
            if (ImGui::Checkbox("visible", &visible)) {
                const uintptr_t packed = static_cast<uintptr_t>(e)
                    | (visible ? (static_cast<uintptr_t>(1) << 32) : static_cast<uintptr_t>(0));
                ctx->GetInputManager()->PushCommand(CommandId::HideEntity, reinterpret_cast<const void*>(packed));
            }
        }
        else if (name == "SpotLight") {
            auto& d = (*arch.get_array<SpotLightComponent>())[row].light_data;
            d.ResolveDistance();
            ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());
        }
        else if (name == "SphereLight") {
            auto& d = (*arch.get_array<SphereLightComponent>())[row].light_data;
            d.ResolveDistance();
            ImGui::Text("Max Distance: %.3f", d.GetMaxDistance());
        }
        else if (name == "DirectLight") {
            auto& d = (*arch.get_array<DirectLightComponent>())[row].light_data;
            for (int c = 0; c < d.cascade_count; ++c) {
                float he = d.CascadeExtent(c);
                float dp = d.CascadeDepth(c);
                ImGui::Text("  c%d: %.1f x %.1f, depth %.1f, texel %.4f",
                    c, 2.0f * he, 2.0f * he, 2.0f * dp, (2.0f * he) / 1024.0f);
            }
        }
    }

    // Единый инспектор сущности: свет — такая же сущность, отдельного «типа выбора» нет. Показываем
    // то, что есть по компонентам: удаление (всегда) + трансформ (если Positions) + коллайдеры + свет.
    void InspectEntity(EngineContext* ctx, ObjectManager* om, SceneData* scene, Entity e)
    {
        ImGui::Text("Entity %u", static_cast<unsigned>(e));

        // Удаление — всегда (в т.ч. у directional-света без трансформа).
        if (DangerButton("Delete", ImVec2(120.0f, 0.0f))) {
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

        // Остальные компоненты — generic по схемам реестра: секция на компонент архетипа
        // (порядок = порядок регистрации, детерминирован), поля рисует DrawComponentFields,
        // производные значения — ComponentExtraUI. Новый зарегистрированный компонент
        // появляется здесь сам, без правки инспектора.
        auto arch_it = scene->entity_to_archetype.find(e);
        auto idx_it  = scene->entity_to_index.find(e);
        if (arch_it != scene->entity_to_archetype.end() && idx_it != scene->entity_to_index.end()) {
            Archetype& arch = *arch_it->second;
            const size_t row = idx_it->second;
            std::string tags;   // теги без данных — одной строкой внизу, не секциями
            for (const ComponentSpec& s : ComponentSpecRegistry::Get().All()) {
                if (!arch.components.count(s.sig_type)) continue;
                // Transform выше (Offset+гизмо), Material редактируется как ассет (вкладка Materials).
                if (s.name == "Transform" || s.name == "Material") continue;
                if (s.fields.empty() && !s.custom_save) {
                    tags += tags.empty() ? s.name : ", " + s.name;
                    continue;
                }
                if (ImGui::CollapsingHeader(s.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    DrawComponentFields(s, arch, row);
                    ComponentExtraUI(ctx, e, s.name, arch, row);
                }
            }
            if (!tags.empty()) { ImGui::Separator(); ImGui::Text("Tags: %s", tags.c_str()); }
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

            // Резолв sp через карту (GetShaderProgram логирует промах — спамил бы каждый кадр).
            auto spIt = sm->GetShaderPrograms().find(spName);
            ShaderProgram* sp = (spIt != sm->GetShaderPrograms().end()) ? spIt->second.get() : nullptr;
            if (!sp) MissingRefMark("shader program not found — renders with fallback");

            // Слоты этого sp; значение — из общей карты по роли (правка отражается во всех sp с этой ролью).
            if (sp)
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
                    // Имя назначено, но текстуры с ним нет (удалена/переименована) → маркер в конце строки.
                    if (!current.empty() && !ctx->GetTextureManager()->GetTextureHandles().count(current))
                        MissingRefMark("texture not found — dummy is used");
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
    enum class PickTarget { None, TexPath, ModelVert, ModelIndex, ShaderVert, ShaderFrag, ShaderComp };
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
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            // old_name = выбранная текстура: если имя изменили — переименование (старую снять в команде).
            ctx->GetInputManager()->PushCommand(CommandId::UpsertTexture,
                new UpsertTextureCmd{ nameBuf, atlasSel, pathBuf, static_cast<uint32_t>(convSel), g_sel.name });
            g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = nameBuf;   // выбор следует за именем
        }
        ImGui::EndDisabled();

        // Удаление существующей текстуры (материалы по её имени → dummy на пересборке).
        if (!g_sel.name.empty()) {
            ImGui::SameLine();
            const bool del = DangerButton("Delete");
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
        if (ImGui::Button("Recreate", ImVec2(160, 0)))
            ctx->GetInputManager()->PushCommand(CommandId::UpsertModel,
                new UpsertModelCmd{ nameBuf, modelBuf, indexBuf, static_cast<uint32_t>(anchorSel) });
        ImGui::EndDisabled();
    }

    // Инспектор/создатель graphics-sp. Одна форма и для правки, и для создания (как текстуры/модели):
    // пустое имя выбора (плитка «+») → sp == nullptr, кнопка «Create»; иначе правка ПЕРЕСОЗДАНИЕМ
    // (delete+create). vs/fs/буферы/слоты/проход/spd копятся в буферах, коммит — одной кнопкой.
    // Push/dispatch в UI НЕ идут (это код) — у sp, созданной из UI, push-констант нет.
    void ShaderInspector(EngineContext* ctx, const std::string& spName)
    {
        // Пустое имя (плитка «+») = создание: НЕ зовём GetShaderProgram (он логирует промах каждый кадр).
        ShaderProgram* sp = spName.empty() ? nullptr : ctx->GetShaderManager()->GetShaderProgram(spName);
        const bool creating = (sp == nullptr);
        InputManager* im = ctx->GetInputManager();
        if (creating) ImGui::TextUnformatted("Shader: (new)");
        else          ImGui::Text("Shader: %s", spName.c_str());

        // ================= Шапка: имя + выбор vs/fs по имени (пересоздание по кнопке) =================
        // Правки копятся в буферах, действие — только по кнопке (delete старой sp + create новой).
        // vs/fs — ИМЕНА из реестров ShaderManager (сами шейдер-данные кодовые, тут только композиция).
        ShaderManager* smgr = ctx->GetShaderManager();
        static char        nameBuf[128] = "";
        static std::string vsSel, fsSel, passSel;
        static std::vector<BufferDataName> vsBufSel, fsBufSel;   // storage-буферы стадий (ключи реестра)
        static std::vector<TextureSlotRole> slotsSel;           // required_slots (взаимоисключающие роли)
        static ShaderProgramDescription spdBuf;
        static std::string syncedFor = "\x01";   // сентинел → синк буферов на смену выбора

        if (spName != syncedFor) {                // синк из выбранной sp (или сброс на дефолты при создании)
            syncedFor = spName;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", spName.c_str());
            if (sp) {
                vsSel = sp->vs_name;
                fsSel = sp->fs_name;
                passSel = sp->associated_render_pass ? sp->associated_render_pass->debug_name : "";
                spdBuf = sp->spd;
                vsBufSel = sp->vertex_shader_buffer_names;     // ссылки по имени — берём как есть
                fsBufSel = sp->fragment_shader_buffer_names;
                slotsSel = sp->required_slots;
            }
            else {   // «+» — чистая форма новой sp
                vsSel.clear(); fsSel.clear(); passSel.clear();
                spdBuf = ShaderProgramDescription{};
                vsBufSel.clear(); fsBufSel.clear(); slotsSel.clear();
            }
        }

        ImGui::TextDisabled(creating ? "Shader program (create)" : "Shader program (compose / rename = recreate)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);

        // Вершинный слот — только вершинники; фрагментный — только фрагментные (фильтр по типу реестра).
        if (ImGui::BeginCombo("Vertex", vsSel.c_str())) {
            for (auto& [n, d] : smgr->GetVertexShaders()) {
                bool is_cur = (n == vsSel);
                if (ImGui::Selectable(n.c_str(), is_cur)) vsSel = n;
                if (is_cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Fragment", fsSel.c_str())) {
            for (auto& [n, d] : smgr->GetFragmentShaders()) {
                bool is_cur = (n == fsSel);
                if (ImGui::Selectable(n.c_str(), is_cur)) fsSel = n;
                if (is_cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // ================= Storage-буферы стадий (в буфер, применяется по кнопке) =================
        // Порядок строк = слоты бинда (BindGPU*StorageBuffers). Раздельно вершинные и фрагментные.
        // Перечень — каноничные ключи реестра (BufferDataName): их же кладём в ссылки sp.
        std::vector<BufferDataName> bufNames;
        for (auto& [k, b] : ctx->GetBufferManager()->GetBuffersData())
            if (b && (g_show_internal || !IsInternalName(b->debug_name))) bufNames.push_back(k);
        std::sort(bufNames.begin(), bufNames.end(),
                  [](BufferDataName a, BufferDataName b) { return std::strcmp(a, b) < 0; });
        BufferListEditor("Vertex buffers",   vsBufSel, bufNames);
        BufferListEditor("Fragment buffers", fsBufSel, bufNames);

        // ================= Слот-роли текстур (в буфер, применяется по кнопке) =================
        RoleListEditor(slotsSel);

        // ================= Проход (в буфер, применяется по кнопке) =================
        ImGui::SeparatorText("Pass");
        if (ImGui::BeginCombo("Pass", passSel.empty() ? "(none)" : passSel.c_str())) {
            for (RenderPassStep* rp : ctx->GetPassManager()->GetOrderedRenderPasses()) {
                bool is_cur = (rp->debug_name == passSel);
                if (ImGui::Selectable(rp->debug_name.c_str(), is_cur)) passSel = rp->debug_name;
                if (is_cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // ================= Pipeline state (spd) — в буфер =================
        ImGui::SeparatorText("Pipeline state (spd)");
        {
            ShaderProgramDescription& d = spdBuf;
            { const char* names[] = { "None", "Front", "Back" }; int v = static_cast<int>(d.cull_mode);
              if (ImGui::Combo("Cull mode", &v, names, 3)) d.cull_mode = static_cast<SDL_GPUCullMode>(v); }
            { const char* names[] = { "Fill", "Wireframe" }; int v = static_cast<int>(d.fill_mode);
              if (ImGui::Combo("Fill mode", &v, names, 2)) d.fill_mode = static_cast<SDL_GPUFillMode>(v); }
            { const char* names[] = { "TriangleList", "TriangleStrip", "LineList", "LineStrip", "PointList" };
              int v = static_cast<int>(d.primitive_type);
              if (ImGui::Combo("Primitive", &v, names, 5)) d.primitive_type = static_cast<SDL_GPUPrimitiveType>(v); }
            ImGui::Checkbox("Depth test",   &d.depth_test);
            ImGui::Checkbox("Depth write",  &d.depth_write);
            ImGui::Checkbox("Stencil test", &d.stencil_test);
            ImGui::Checkbox("Color blend",  &d.color_blend);
            ImGui::Checkbox("Depth bias", &d.rasterizer_bias.enable_depth_bias);
            if (d.rasterizer_bias.enable_depth_bias) {
                ImGui::DragFloat("Bias constant", &d.rasterizer_bias.depth_bias_constant_factor, 0.05f);
                ImGui::DragFloat("Bias slope",    &d.rasterizer_bias.depth_bias_slope_factor, 0.05f);
                ImGui::DragFloat("Bias clamp",    &d.rasterizer_bias.depth_bias_clamp, 0.05f);
            }
        }

        // ===== Одна кнопка на ВСЮ композицию sp (имя/vs/fs/буферы/слоты/проход/spd) =====
        // Создание: имя обязано быть свободным (иначе кнопка гаснет — не молчаливая перезапись).
        ImGui::Separator();
        const bool nameFree = !smgr->GetShaderPrograms().count(nameBuf);
        const bool ready = nameBuf[0] && !vsSel.empty() && !fsSel.empty() && !passSel.empty()
            && (!creating || nameFree);
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(creating ? "Create" : "Apply / Recreate", ImVec2(160, 0))) {
            im->PushCommand(CommandId::RecreateShader,
                new RecreateShaderCmd{ spName, nameBuf, vsSel, fsSel, passSel, spdBuf, vsBufSel, fsBufSel, slotsSel });
            g_sel = Selection{}; g_sel.kind = SelKind::Shader; g_sel.name = nameBuf;   // выбор на созданную/переименованную
        }
        ImGui::EndDisabled();
        if (creating && nameBuf[0] && !nameFree) { ImGui::SameLine(); ImGui::TextDisabled("(name taken)"); }

        // ================= Удаление (внизу, только у существующей) =================
        // Пайплайн в отложенное удаление, шейдеры релизятся по refcount, материалы → fallback.
        if (!creating) {
            ImGui::Separator();
            if (DangerButton("Delete shader")) {
                im->PushCommand(CommandId::DeleteShader, new RebuildShaderPipelineCmd{ spName });
                g_sel = Selection{};
                return;
            }
        }
    }
    // Редактор списка storage-буферов стадии (порядок строк = слоты бинда). Каждый слот — свой
    // выпадающий список: буфер можно заменить на ЛЮБОЙ другой (в отличие от взаимоисключающих
    // семантик pull в VsdEditor — там уже добавленную нельзя выбрать повторно). Выбор не исключающий:
    // один буфер может стоять в нескольких слотах. "x" справа убирает слот, "+ buffer" — добавляет
    // новый в конец. Правит list на месте; коммит — общей кнопкой sp.
    void BufferListEditor(const char* label, std::vector<BufferDataName>& list,
                          const std::vector<BufferDataName>& avail)
    {
        ImGui::PushID(label);
        ImGui::SeparatorText(label);
        int rm_at = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);   // оставить место под "x"
            if (ImGui::BeginCombo("##buf", list[i])) {
                for (BufferDataName b : avail) {
                    bool is_cur = (b == list[i]);
                    if (ImGui::Selectable(b, is_cur)) list[i] = b;
                    if (is_cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) rm_at = i;
            ImGui::PopID();
        }
        if (rm_at >= 0) list.erase(list.begin() + rm_at);

        if (ImGui::BeginCombo("+ buffer", "(add)")) {
            for (BufferDataName b : avail)                       // не исключающий: дубликаты допустимы
                if (ImGui::Selectable(b)) list.push_back(b);
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    // Редактор слот-ролей sp (required_slots). В отличие от буферов роли ВЗАИМОИСКЛЮЧАЮЩИЕ: одна
    // роль = один слот, поэтому дропдаун каждого слота и "+ slot" показывают только ещё не занятые
    // роли (+ текущую самого слота). Порядок не влияет — стрелок нет. "x" убирает слот.
    void RoleListEditor(std::vector<TextureSlotRole>& list)
    {
        static const TextureSlotRole kRoles[] = {
            TextureSlotRole::Albedo, TextureSlotRole::Normal, TextureSlotRole::ORM, TextureSlotRole::Emissive,
            TextureSlotRole::Custom0, TextureSlotRole::Custom1, TextureSlotRole::Custom2, TextureSlotRole::Custom3,
            TextureSlotRole::Custom4, TextureSlotRole::Custom5, TextureSlotRole::Custom6, TextureSlotRole::Custom7,
        };
        ImGui::PushID("roles");
        ImGui::SeparatorText("Texture slots");
        auto used_elsewhere = [&](TextureSlotRole r, int self) {
            for (int j = 0; j < static_cast<int>(list.size()); ++j) if (j != self && list[j] == r) return true;
            return false;
        };
        int rm_at = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);   // место под "x"
            if (ImGui::BeginCombo("##role", RoleName(list[i]))) {
                for (TextureSlotRole r : kRoles) {
                    if (used_elsewhere(r, i)) continue;                 // занятую другим слотом не предлагаем
                    bool is_cur = (r == list[i]);
                    if (ImGui::Selectable(RoleName(r), is_cur)) list[i] = r;
                    if (is_cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) rm_at = i;
            ImGui::PopID();
        }
        if (rm_at >= 0) list.erase(list.begin() + rm_at);

        if (ImGui::BeginCombo("+ slot", "(add)")) {                     // только ещё не добавленные роли
            for (TextureSlotRole r : kRoles) {
                if (std::find(list.begin(), list.end(), r) != list.end()) continue;
                if (ImGui::Selectable(RoleName(r))) list.push_back(r);
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    // VertexSemantic -> строка (набор фиксирован; реестра раскладок пока нет).
    const char* SemName(VertexSemantic s)
    {
        switch (s) {
        case POSITION: return "POSITION";
        case UV:       return "UV";
        case NORMAL:   return "NORMAL";
        case TANGENT:  return "TANGENT";
        default:       return "?";
        }
    }

    // Общий приём пути из файл-диалога для форм SD (по своему PickTarget-полю).
    bool TakePickedPath(PickTarget want, char* dst, size_t n)
    {
        if (g_pick_target == want && g_picked_ready.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            std::snprintf(dst, n, "%s", g_picked_path.c_str());
            g_pick_target = PickTarget::None;
            return true;
        }
        return false;
    }

    static const SDL_DialogFileFilter kHlslFilters[] = { { "HLSL", "hlsl" }, { "All files", "*" } };

    // Форма фрагментного шейдера (create/edit = Upsert по имени, как текстура/модель).
    void FsdEditor(EngineContext* ctx)
    {
        static char nameBuf[128] = "", pathBuf[512] = "";
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            FragmentShaderData* d = ctx->GetShaderManager()->GetFragmentShader(g_sel.name);
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
        }
        TakePickedPath(PickTarget::ShaderFrag, pathBuf, sizeof pathBuf);

        ImGui::TextDisabled("Fragment shader (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##fsd")) OpenFileDialog(PickTarget::ShaderFrag, kHlslFilters, 2);

        const bool ready = nameBuf[0] && pathBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertFragmentShader,
                new UpsertFragmentShaderCmd{ nameBuf, pathBuf, g_sel.name });
            g_sel = Selection{}; g_sel.kind = SelKind::Fsd; g_sel.name = nameBuf;
        }
        ImGui::EndDisabled();
        if (!g_sel.name.empty()) {
            ImGui::SameLine();
            const bool used = ctx->GetShaderManager()->IsFragmentShaderUsed(g_sel.name);
            ImGui::BeginDisabled(used);   // используемый SD удалять запрещено (см. ShaderManager)
            if (DangerButton("Delete")) {
                ctx->GetInputManager()->PushCommand(CommandId::DeleteFragmentShader, new ShaderDataNameCmd{ g_sel.name });
                g_sel = Selection{};
            }
            ImGui::EndDisabled();
            if (used) { ImGui::SameLine(); ImGui::TextDisabled("(used by sp)"); }
        }
    }

    // Форма compute-шейдера (аналогично FSD; source_path у CSD не храним).
    void CsdEditor(EngineContext* ctx)
    {
        static char nameBuf[128] = "", pathBuf[512] = "";
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            ComputeShaderData* d = ctx->GetShaderManager()->GetComputeShader(g_sel.name);   // путь теперь хранится (см. CSD)
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
        }
        TakePickedPath(PickTarget::ShaderComp, pathBuf, sizeof pathBuf);

        ImGui::TextDisabled("Compute shader (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##csd")) OpenFileDialog(PickTarget::ShaderComp, kHlslFilters, 2);

        const bool ready = nameBuf[0] && pathBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertComputeShader,
                new UpsertComputeShaderCmd{ nameBuf, pathBuf, g_sel.name });
            g_sel = Selection{}; g_sel.kind = SelKind::Csd; g_sel.name = nameBuf;
        }
        ImGui::EndDisabled();
        if (!g_sel.name.empty()) {
            ImGui::SameLine();
            const bool used = ctx->GetShaderManager()->IsComputeShaderUsed(g_sel.name);
            ImGui::BeginDisabled(used);   // используемый SD удалять запрещено (см. ShaderManager)
            if (DangerButton("Delete")) {
                ctx->GetInputManager()->PushCommand(CommandId::DeleteComputeShader, new ShaderDataNameCmd{ g_sel.name });
                g_sel = Selection{};
            }
            ImGui::EndDisabled();
            if (used) { ImGui::SameLine(); ImGui::TextDisabled("(used by sp)"); }
        }
    }

    // Форма вершинного шейдера: имя + путь + РАСКЛАДКА (pull) — семантики добавляются/удаляются и
    // переставляются стрелками (реестра форматов пока нет; формат фиксирован FMT_PosUVNormal).
    void VsdEditor(EngineContext* ctx)
    {
        static char nameBuf[128] = "", pathBuf[512] = "";
        static std::vector<VertexSemantic> pull;
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            VertexShaderData* d = ctx->GetShaderManager()->GetVertexShader(g_sel.name);
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
            pull.clear();
            // Все слоты (со стримами пула биндингов несколько — Pos/UV/NormTan): pull формы =
            // объединение семантик по слотам, как в манифесте.
            if (d) for (const auto& b : d->bindings)
                for (VertexSemantic s : b.pull) pull.push_back(s);
            if (pull.empty()) pull.push_back(POSITION);                       // дефолт для новой
        }
        TakePickedPath(PickTarget::ShaderVert, pathBuf, sizeof pathBuf);

        ImGui::TextDisabled("Vertex shader (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##vsd")) OpenFileDialog(PickTarget::ShaderVert, kHlslFilters, 2);

        // Раскладка pull: строки со стрелками up/dn (перестановка) и x (удаление).
        ImGui::SeparatorText("Vertex layout (pull)");
        int mv_from = -1, mv_to = -1, rm_at = -1;
        for (int i = 0; i < static_cast<int>(pull.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)   && i > 0)                               { mv_from = i; mv_to = i - 1; }
            ImGui::SameLine();
            if (ImGui::ArrowButton("dn", ImGuiDir_Down) && i < static_cast<int>(pull.size()) - 1) { mv_from = i; mv_to = i + 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) rm_at = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(SemName(pull[i]));
            ImGui::PopID();
        }
        if (mv_from >= 0) std::swap(pull[mv_from], pull[mv_to]);
        if (rm_at  >= 0) pull.erase(pull.begin() + rm_at);

        // Добавить недостающую семантику (набор фиксирован).
        static const VertexSemantic kSems[] = { POSITION, UV, NORMAL, TANGENT };
        if (ImGui::BeginCombo("+ field", "(add)")) {
            for (VertexSemantic sem : kSems) {
                if (std::find(pull.begin(), pull.end(), sem) != pull.end()) continue;
                if (ImGui::Selectable(SemName(sem))) pull.push_back(sem);
            }
            ImGui::EndCombo();
        }

        const bool ready = nameBuf[0] && pathBuf[0] && !pull.empty();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertVertexShader,
                new UpsertVertexShaderCmd{ nameBuf, pathBuf, g_sel.name, pull });
            g_sel = Selection{}; g_sel.kind = SelKind::Vsd; g_sel.name = nameBuf;
        }
        ImGui::EndDisabled();
        if (!g_sel.name.empty()) {
            ImGui::SameLine();
            const bool used = ctx->GetShaderManager()->IsVertexShaderUsed(g_sel.name);
            ImGui::BeginDisabled(used);   // используемый SD удалять запрещено (см. ShaderManager)
            if (DangerButton("Delete")) {
                ctx->GetInputManager()->PushCommand(CommandId::DeleteVertexShader, new ShaderDataNameCmd{ g_sel.name });
                g_sel = Selection{};
            }
            ImGui::EndDisabled();
            if (used) { ImGui::SameLine(); ImGui::TextDisabled("(used by sp)"); }
        }
    }
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

    case SelKind::Vsd: VsdEditor(ctx); break;
    case SelKind::Fsd: FsdEditor(ctx); break;
    case SelKind::Csd: CsdEditor(ctx); break;

    default:
        ImGui::TextDisabled("Nothing selected.");
        break;
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
