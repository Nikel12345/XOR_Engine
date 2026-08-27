#include "PCH.h"
#include "BaseComponents.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "imgui_internal.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "InputCommands.h"
// EngineContext.h держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "ObjectManager.h"
#include "CameraManager.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ShaderManager.h"
#include "BatchBuilder.h"
#include "ParamsSpec.h"
#include "ComponentSerializer.h"
#include "UI_ComponentEditor.h"
#include "RenderManager.h"
#include "UI_Yoga.h"
#include "ImGuizmo.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <mutex>
#include <atomic>
#include <filesystem>

using namespace ShaderBase;   // VertexSemantic в редакторе pull вершинника
#include <SDL3/SDL_dialog.h>

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

    // ShaderInspector дёргает редакторы списков буферов/слот-ролей, определённые ниже (рядом с формами SD).
    void BufferListEditor(const char* label, std::vector<BufferDataName>& list, const std::vector<BufferDataName>& avail);
    void RoleListEditor(std::vector<TextureSlotRole>& list);

    // Единый инспектор сущности: свет — такая же сущность, отдельного «типа выбора» нет. Шапка
    // (удаление + перенос + рамки коллайдеров) — про ЭНТИТИ, а не про компонент; сами компоненты
    // рисует общий с формой создания вид (ui::DrawEntityComponents).
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

        auto arch_it = scene->entity_to_archetype.find(e);
        auto idx_it  = scene->entity_to_index.find(e);
        if (arch_it == scene->entity_to_archetype.end() || idx_it == scene->entity_to_index.end()) return;
        Archetype& arch = *arch_it->second;
        const size_t row = idx_it->second;

        // Debug-рамки коллайдеров (дети): галочка visible → HideEntity.
        auto kids_it = scene->children.find(e);
        if (kids_it != scene->children.end() && !kids_it->second.empty()) {
            ImGui::SeparatorText("Debug colliders");
            for (Entity c : kids_it->second) {
                if (!om->Has<DrawComponent>(scene, c)) continue;
                bool visible = om->GetComponent<DrawComponent>(scene, c).visible;
                char clabel[40]; snprintf(clabel, sizeof(clabel), "visible (collider %u)", static_cast<unsigned>(c));
                if (ImGui::Checkbox(clabel, &visible))   // то же поле схемы, только у ребёнка
                    ctx->GetInputManager()->PushCommand(CommandId::HideEntity,
                        new FieldEditCmd{ c, "Draw", "visible", visible ? 1.0 : 0.0, {} });
            }
        }

        DrawEntityComponents(EditTarget{ ctx, e }, arch, row);
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

        // Материал = набор sp (проходов), и у КАЖДОЙ свои данные: слоты диктует её required_slots,
        // params — её собственный блоб (SpBinding). Текстуры при этом ОБЩИЕ и ключуются ролью:
        // правка под одним sp видна под другим. Params — нет: они адресованы конкретной программе.
        ImGui::SeparatorText("Shaders");
        const auto& specs = ParamsSpecRegistry::Materials().All();

        std::vector<std::string> texNames;                   // значения комбобокса текстур — по алфавиту
        for (auto& [n, h] : ctx->GetTextureManager()->GetTextureHandles())
            if (g_show_internal || !IsInternalName(n)) texNames.push_back(n);
        std::sort(texNames.begin(), texNames.end());

        for (size_t si = 0; si < mat->shader_programs.size(); ++si) {
            SpBinding& binding = mat->shader_programs[si];
            const std::string spName = binding.sp;
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

            // -- params ЭТОЙ sp -- Смена ТИПА = дефолтный блоб типа из реестра. Правка ПОЛЕЙ идёт
            // in-place и дерево не трогает (ключ узла — адрес блоба, а не байты), но смена типа
            // адрес заводит или убирает, то есть меняет сам ключ -> батчи пересобрать.
            // Список типов — весь реестр: и движковые, и зарегистрированные кодом игры.
            const ParamsSpec* cur = ParamsSpecRegistry::Materials().ByName(binding.params_type);
            // Тип назван, но не зарегистрирован (сцена от сборки, где он был) — не молчим: блоб
            // рисовать нечем, а SaveScene его не сохранит.
            const bool unknown_type = !binding.params_type.empty() && !cur;
            if (ImGui::BeginCombo("Params", cur ? cur->name.c_str()
                                                : (unknown_type ? binding.params_type.c_str() : "(none)"))) {
                if (ImGui::Selectable("(none)", binding.params_type.empty()) && !binding.params_type.empty()) {
                    ClearMaterialParams(&binding);
                    ctx->GetBatchBuilder()->SetDirtyBatches(true);
                }
                for (const ParamsSpec& s : specs) {
                    const bool is_cur = (&s == cur);
                    if (ImGui::Selectable(s.name.c_str(), is_cur) && !is_cur) {
                        ApplyMaterialParamsSpec(&binding, s);
                        ctx->GetBatchBuilder()->SetDirtyBatches(true);
                    }
                    if (is_cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (unknown_type)
                MissingRefMark("params type is not registered — fields cannot be edited and will NOT be saved");

            if (!binding.params || binding.params->empty()) ImGui::TextDisabled("(no params)");
            else if (!cur)             ImGui::TextDisabled("(%zu bytes, unknown layout)", binding.params->size());
            else if (cur->custom_edit) cur->custom_edit(binding.params->data());   // escape hatch типа
            else                       DrawParamsFields(*cur, *binding.params);

            ImGui::PopID();
            ImGui::Separator();
        }

        // Добавить sp (перечень graphics sp, ещё не добавленных материалу).
        if (ImGui::BeginCombo("+ Shader", "(add)")) {
            for (auto& [spn, spp] : sm->GetShaderPrograms()) {
                bool present = false;
                for (auto& b : mat->shader_programs) if (b.sp == spn) { present = true; break; }
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
    //     поле заполнять (у формы модели два пути) — активная форма забирает только «своё».
    //
    //     Пути ресурсов ХРАНЯТСЯ ОТНОСИТЕЛЬНО КОРНЯ ПРОЕКТА (current_path(): в dev его ставит cmake,
    //     в install — папка exe), иначе scene.json не переносится через git/установку. Поэтому сырой
    //     путь из диалога (диалог отдаёт абсолютный) прогоняется через финализацию ProcessPendingPick:
    //       • файл ВНУТРИ проекта → относительный путь;
    //       • файл СНАРУЖИ (другой диск / вне дерева) → лог + второй диалог (выбор папки внутри
    //         проекта) → копия файла туда → относительный путь до копии.
    //     Абсолютный путь наружу проекта НЕ сохраняется никогда (git его не заберёт). ---
    enum class PickTarget { None, TexPath, ModelVert, ModelIndex, ShaderVert, ShaderFrag, ShaderComp };
    PickTarget        g_pick_target = PickTarget::None;
    std::mutex        g_pick_mtx;                 // охраняет все строковые буферы ниже
    std::string       g_picked_path;             // ФИНАЛЬНЫЙ относительный путь для форм (ставит финализация)
    std::atomic<bool> g_picked_ready{ false };

    std::string       g_raw_path;                // сырой (абсолютный) путь из первого диалога
    std::atomic<bool> g_raw_ready{ false };
    std::string       g_copy_src;                // исходник, ждущий копирования (файл вне проекта)
    std::string       g_folder_path;             // папка назначения из второго диалога (пусто = отмена)
    std::atomic<bool> g_folder_ready{ false };

    // Первый диалог (выбор файла): кладём СЫРОЙ путь — финализация на кадре (см. ProcessPendingPick).
    void SDLCALL OnFilePicked(void*, const char* const* filelist, int)
    {
        if (filelist && filelist[0]) {                 // пусто = отмена, nullptr = ошибка
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            g_raw_path = filelist[0];
            g_raw_ready.store(true, std::memory_order_release);
        }
    }

    // Второй диалог (выбор папки назначения для копии файла извне проекта). Отмену тоже отмечаем
    // готовой — финализация на кадре снимет ожидание и отменит операцию.
    void SDLCALL OnFolderPicked(void*, const char* const* filelist, int)
    {
        std::lock_guard<std::mutex> lk(g_pick_mtx);
        g_folder_path = (filelist && filelist[0]) ? filelist[0] : std::string();
        g_folder_ready.store(true, std::memory_order_release);
    }

    // Открыть нативный диалог выбора ФАЙЛА, пометив целевое поле (SDL требует main-поток; результат —
    // потокобезопасно). Финализирует ProcessPendingPick.
    void OpenFileDialog(PickTarget target, const SDL_DialogFileFilter* filters, int nfilters)
    {
        g_pick_target = target;
        SDL_ShowOpenFileDialog(OnFilePicked, nullptr, nullptr, filters, nfilters, nullptr, false);
    }

    // SDL отдаёт пути в UTF-8, а MSVC std::filesystem трактует узкую строку как ACP → кириллица
    // (G:\контент\...) бьётся и файл «не находится». Строим path из UTF-8 явно (C++20/23 aware),
    // и обратно path→UTF-8 для хранения/JSON.
    std::filesystem::path PathFromU8(const std::string& s)
    {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
    }
    std::string U8FromPath(const std::filesystem::path& p)
    {
        std::u8string u = p.generic_u8string();
        return std::string(u.begin(), u.end());
    }

    // «Внутри проекта» = relative(path, root) существует и не убегает вверх ('..'). Кроссдисковый путь
    // relative() отдаёт пустым — тоже «снаружи». Пустая строка возврата = снаружи (UTF-8, '/').
    std::string RelativeInsideProject(const std::filesystem::path& p, const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, root, ec);
        if (ec) return {};
        std::string s = U8FromPath(rel);                       // '/' — единообразно и портируемо в JSON
        if (s.empty() || s.rfind("..", 0) == 0) return {};     // вне дерева проекта
        return s;
    }

    // Раз в кадр (верх DrawInspector): превращаем сырой путь из диалога в финальный ОТНОСИТЕЛЬНЫЙ,
    // при необходимости через копирование извне проекта. Только по завершении ставим g_picked_ready —
    // формы (TakePickedPath / инлайн-приёмы) забирают уже готовый относительный путь.
    void ProcessPendingPick()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::current_path(ec);            // корень проекта (cmake в dev / exe в install)

        // Этап 1: свежий файл из первого диалога.
        if (g_raw_ready.exchange(false, std::memory_order_acquire)) {
            std::string raw;
            { std::lock_guard<std::mutex> lk(g_pick_mtx); raw = g_raw_path; }

            std::string rel = RelativeInsideProject(PathFromU8(raw), root);
            if (!rel.empty()) {                                // внутри проекта — сразу относительный
                std::lock_guard<std::mutex> lk(g_pick_mtx);
                g_picked_path = rel;
                g_picked_ready.store(true, std::memory_order_release);
            } else {                                           // снаружи — лог + запрос папки для копии
                SDL_Log("[Path] '%s' is outside the project (%s) - pick a folder INSIDE the project to copy it into",
                        raw.c_str(), U8FromPath(root).c_str());
                { std::lock_guard<std::mutex> lk(g_pick_mtx); g_copy_src = raw; }
                SDL_ShowOpenFolderDialog(OnFolderPicked, nullptr, nullptr, root.string().c_str(), false);
            }
        }

        // Этап 2: выбрана папка назначения — копируем и берём относительный путь до копии.
        if (g_folder_ready.exchange(false, std::memory_order_acquire)) {
            std::string src, folder;
            { std::lock_guard<std::mutex> lk(g_pick_mtx); src = g_copy_src; folder = g_folder_path; g_copy_src.clear(); }

            if (folder.empty()) {                              // отмена выбора папки
                SDL_Log("[Path] copy cancelled - path left unset");
                g_pick_target = PickTarget::None;
                return;
            }
            fs::path srcP = PathFromU8(src);
            fs::path dstP = PathFromU8(folder) / srcP.filename();
            fs::copy_file(srcP, dstP, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                SDL_Log("[Path] copy failed: %s -> %s (%s)",
                        src.c_str(), U8FromPath(dstP).c_str(), ec.message().c_str());
                g_pick_target = PickTarget::None;
                return;
            }
            std::string rel = RelativeInsideProject(dstP, root);
            if (rel.empty()) {                                 // выбрал папку вне проекта — путь не сохраняем
                SDL_Log("[Path] chosen folder is outside the project - path not stored (file copied to %s)",
                        U8FromPath(dstP).c_str());
                g_pick_target = PickTarget::None;
                return;
            }
            SDL_Log("[Path] copied: %s -> %s", src.c_str(), rel.c_str());
            std::lock_guard<std::mutex> lk(g_pick_mtx);
            g_picked_path = rel;
            g_picked_ready.store(true, std::memory_order_release);
        }
    }

    // Куб отличает ТИП АТЛАСА — тот же признак, по которому его пишет SaveScene и грузит LoadScene.
    // Своего «я куб» у хэндла нет и не нужно: куб — обычная текстура на 6 слоях cube-атласа.
    bool IsCubeAtlas(const TextureAtlas* a) {
        return a && (a->texture_type == SDL_GPU_TEXTURETYPE_CUBE
                  || a->texture_type == SDL_GPU_TEXTURETYPE_CUBE_ARRAY);
    }

    // Форма создания/редактирования текстуры (одна и та же — см. upsert delete+create). Ничего не
    // происходит по-символьно: правки копятся в буферах, действие только по кнопке.
    void TextureEditor(EngineContext* ctx)
    {
        static char        nameBuf[128] = "";
        static char        pathBuf[512] = "";
        static std::string atlasSel;
        static ChannelConvention convSel = ChannelConvention::AsIs;
        static bool        cubeSel = false;   // исходник — крест 4×3, а не плоская картинка
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
                    cubeSel = IsCubeAtlas(h->atlas);
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

        // Вид исходника. Cube = горизонтальный крест 4×3 одним файлом: режется на 6 граней и ложится
        // слоями cube-атласа (EngineContext::CreateCubeMapTexture). Плоские и cube-атласы не
        // взаимозаменяемы, поэтому переключение вида сбрасывает выбор атласа, а не оставляет
        // заведомо негодный.
        int kind = cubeSel ? 1 : 0;
        ImGui::RadioButton("Flat", &kind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Cube (cross 4x3)", &kind, 1);
        if ((kind == 1) != cubeSel) { cubeSel = (kind == 1); atlasSel.clear(); }

        // Атлас — дропдаун существующих (общий фильтр служебных с браузером), отфильтрованный по виду.
        if (ImGui::BeginCombo("Atlas", atlasSel.empty() ? "(select)" : atlasSel.c_str())) {
            for (auto& [an, a] : ctx->GetTextureManager()->GetAtlases()) {
                if (!g_show_internal && IsInternalName(an)) continue;
                if (IsCubeAtlas(a.get()) != cubeSel) continue;
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

        // Конвенция каналов исходника (enum → switch, см. ConvName). К кубу не применяется —
        // нормализуют каналы только материальные карты, поэтому для него поля нет вовсе.
        static const ChannelConvention kConvs[] = {
            ChannelConvention::AsIs, ChannelConvention::SmoothnessInGreen, ChannelConvention::DepthInAlpha
        };
        if (!cubeSel && ImGui::BeginCombo("Channels", ConvName(convSel))) {
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
                new UpsertTextureCmd{ nameBuf, atlasSel, pathBuf,
                                      cubeSel ? 0u : static_cast<uint32_t>(convSel),   // у куба конвенции нет
                                      g_sel.name, cubeSel });
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
    // Состояние прохода: тот же generic-рендерер схемы, что и у params материала. Схему ищем ПО
    // ИМЕНИ из самого шага (state_type) — в реестре ПРОХОДОВ, не материалов (см. ParamsSpec.h).
    // Правка мутирует байты на месте: тело прохода отдаёт вниз указатель на этот же блоб, а
    // UI-поток и есть рендер-поток (UI рисуется внутри RenderFunc) — команда не нужна.
    void DrawPassState(std::vector<uint8_t>& state, const std::string& state_type)
    {
        if (state_type.empty()) {
            ImGui::TextDisabled("(no editable state; push/dispatch are code)");
            return;
        }
        const ParamsSpec* spec = ParamsSpecRegistry::Passes().ByName(state_type);
        if (!spec) {
            ImGui::TextDisabled("state type '%s' is not registered", state_type.c_str());
            return;
        }
        ImGui::SeparatorText(state_type.c_str());
        DrawParamsFields(*spec, state);
    }

    // Шаг кадра по имени-ключу реестра. Пространство имён у пассов и препассов общее
    // (CreateComputePass отказывает на занятое имя), поэтому порядок проверок однозначен.
    void InspectPass(EngineContext* ctx, const std::string& name)
    {
        PassManager* pmgr = ctx->GetPassManager();
        ImGui::Text("Pass: %s", name.c_str());

        if (auto it = pmgr->GetRenderPasses().find(name); it != pmgr->GetRenderPasses().end()) {
            ImGui::TextDisabled("render pass, index %d", it->second->pass_index);
            DrawPassState(it->second->state, it->second->state_type);
            return;
        }
        if (auto it = pmgr->GetComputePasses().find(name); it != pmgr->GetComputePasses().end()) {
            ImGui::TextDisabled("compute pass, index %d", it->second->pass_index);
            DrawPassState(it->second->state, it->second->state_type);
            return;
        }
        if (auto it = pmgr->GetComputePrepasses().find(name); it != pmgr->GetComputePrepasses().end()) {
            ImGui::TextDisabled("compute prepass, index %d", it->second->pass_index);
            DrawPassState(it->second->state, it->second->state_type);
            return;
        }
        ImGui::TextDisabled("(pass not found)");
    }

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
                passSel = sp->render_pass_name;
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

    // Дефайны компиляции правятся ОДНОЙ строкой "NAME=VALUE NAME2" через пробел: набор редкий и
    // короткий, а построчный редактор потребовал бы char-буфер на строку (ImGui пишет в char[]).
    // Пробелов внутри препроцессорной константы не бывает, поэтому разбор по пробелу безопасен.
    std::string DefinesToStr(const std::vector<ShaderDefine>& defs)
    {
        std::string s;
        for (const ShaderDefine& d : defs) {
            if (!s.empty()) s += ' ';
            s += d.name;
            if (!d.value.empty()) { s += '='; s += d.value; }
        }
        return s;
    }

    ShaderDefines DefinesFromStr(const char* s)
    {
        ShaderDefines out;
        for (const char* p = s; *p; ) {
            while (*p == ' ' || *p == '	') ++p;
            const char* b = p;
            while (*p && *p != ' ' && *p != '	') ++p;
            if (p == b) continue;
            const std::string tok(b, p - b);
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) out.push_back({ tok, {} });   // без значения = 1
            else                         out.push_back({ tok.substr(0, eq), tok.substr(eq + 1) });
        }
        return out;
    }

    // Поле дефайнов для всех трёх форм SD.
    void DefinesField(char* buf, size_t n)
    {
        ImGui::InputText("Defines", buf, n);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("NAME=VALUE, space separated. Without =VALUE the define equals 1.");
    }

    // Форма фрагментного шейдера (create/edit = Upsert по имени, как текстура/модель).
    void FsdEditor(EngineContext* ctx)
    {
        static char nameBuf[128] = "", pathBuf[512] = "", defsBuf[512] = "";
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            FragmentShaderData* d = ctx->GetShaderManager()->GetFragmentShader(g_sel.name);
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
            std::snprintf(defsBuf, sizeof defsBuf, "%s", d ? DefinesToStr(d->defines).c_str() : "");
        }
        TakePickedPath(PickTarget::ShaderFrag, pathBuf, sizeof pathBuf);

        ImGui::TextDisabled("Fragment shader (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##fsd")) OpenFileDialog(PickTarget::ShaderFrag, kHlslFilters, 2);
        DefinesField(defsBuf, sizeof defsBuf);

        const bool ready = nameBuf[0] && pathBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertFragmentShader,
                new UpsertFragmentShaderCmd{ nameBuf, pathBuf, g_sel.name, DefinesFromStr(defsBuf) });
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
        static char nameBuf[128] = "", pathBuf[512] = "", defsBuf[512] = "";
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            ComputeShaderData* d = ctx->GetShaderManager()->GetComputeShader(g_sel.name);   // путь теперь хранится (см. CSD)
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
            std::snprintf(defsBuf, sizeof defsBuf, "%s", d ? DefinesToStr(d->defines).c_str() : "");
        }
        TakePickedPath(PickTarget::ShaderComp, pathBuf, sizeof pathBuf);

        ImGui::TextDisabled("Compute shader (create / edit)");
        ImGui::InputText("Name", nameBuf, sizeof nameBuf);
        ImGui::InputText("Path", pathBuf, sizeof pathBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##csd")) OpenFileDialog(PickTarget::ShaderComp, kHlslFilters, 2);
        DefinesField(defsBuf, sizeof defsBuf);

        const bool ready = nameBuf[0] && pathBuf[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertComputeShader,
                new UpsertComputeShaderCmd{ nameBuf, pathBuf, g_sel.name, DefinesFromStr(defsBuf) });
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

    // Форма вершинного шейдера: имя + путь + ПУЛ + раскладка (pull). Набор доступных семантик
    // предлагает сам пул — фиксированной четвёрки POSITION/UV/NORMAL/TANGENT больше нет.
    void VsdEditor(EngineContext* ctx)
    {
        static char nameBuf[128] = "", pathBuf[512] = "", defsBuf[512] = "";
        static std::vector<VertexSemantic> pull;
        static std::string poolSel;
        static std::string syncedFor = "\x01";
        if (g_sel.name != syncedFor) {
            syncedFor = g_sel.name;
            std::snprintf(nameBuf, sizeof nameBuf, "%s", g_sel.name.c_str());
            VertexShaderData* d = ctx->GetShaderManager()->GetVertexShader(g_sel.name);
            std::snprintf(pathBuf, sizeof pathBuf, "%s", d ? d->source_path.c_str() : "");
            std::snprintf(defsBuf, sizeof defsBuf, "%s", d ? DefinesToStr(d->defines).c_str() : "");
            poolSel = d ? d->pool_name : std::string();
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
        DefinesField(defsBuf, sizeof defsBuf);

        // Пул выбирается ЯВНО: он задаёт и набор доступных семантик, и порядок слотов. У новой
        // формы — дефолтный, чтобы обычный случай был на клик короче.
        ModelManager* mm = ctx->GetModelManager();
        if (poolSel.empty() && mm->DefaultPool()) poolSel = mm->DefaultPool()->Name();
        GeometryPool* pool = nullptr;
        if (auto pit = mm->GetPools().find(poolSel); pit != mm->GetPools().end()) pool = pit->second.get();

        if (ImGui::BeginCombo("Pool", poolSel.c_str())) {
            for (auto& [pname, pp] : mm->GetPools())
                if (ImGui::Selectable(pname.c_str(), pname == poolSel)) poolSel = pname;
            ImGui::EndCombo();
        }

        // Раскладка pull: строка на семантику с кнопкой удаления. Стрелок перестановки тут НЕТ и
        // быть не должно: порядок pull никуда не доезжает — слоты задаёт таблица стримов пула
        // (StreamsForSemantics обходит её, а не pull), и сохранение пишет каноничный порядок.
        ImGui::SeparatorText("Vertex layout (pull)");
        int rm_at = -1;
        for (int i = 0; i < static_cast<int>(pull.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::SmallButton("x")) rm_at = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(SemName(pull[i]));
            ImGui::PopID();
        }
        if (rm_at >= 0) pull.erase(pull.begin() + rm_at);

        // Добавить недостающую семантику — только из того, что эта раскладка вообще даёт.
        if (pool && ImGui::BeginCombo("+ field", "(add)")) {
            for (VertexSemantic sem : pool->AvailableSemantics()) {
                if (std::find(pull.begin(), pull.end(), sem) != pull.end()) continue;
                if (ImGui::Selectable(SemName(sem))) pull.push_back(sem);
            }
            ImGui::EndCombo();
        }

        // Правятся семантики, а биндятся СТРИМЫ, и это не один к одному (NORMAL и TANGENT — один
        // стрим, снять только один из них ничего не изменит). Показываем результат резолва, чтобы
        // схлопывание было видно, а не удивляло.
        if (pool) {
            std::string slots;
            for (const GeometryPool::Stream* st : pool->StreamsForSemantics(pull)) {
                if (!slots.empty()) slots += ", ";
                slots += st->buffer_name;
            }
            ImGui::TextDisabled("-> slots: %s", slots.empty() ? "(none)" : slots.c_str());
        }

        const bool ready = nameBuf[0] && pathBuf[0] && !pull.empty() && pool;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Recreate", ImVec2(160, 0))) {
            ctx->GetInputManager()->PushCommand(CommandId::UpsertVertexShader,
                new UpsertVertexShaderCmd{ nameBuf, pathBuf, g_sel.name, poolSel, pull, DefinesFromStr(defsBuf) });
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

    ProcessPendingPick();   // сырой путь из диалога → финальный относительный (до приёма формами)

    ObjectManager* om = ctx->GetObjectManager();
    SceneData* scene = om->GetActiveScene();

    // Режим гизмо — только если у выбранной сущности есть Positions (иначе двигать нечего).
    if (g_sel.kind == SelKind::Entity && scene && om->Has<Positions>(scene, g_sel.entity)) {
        ImGui::TextUnformatted("Gizmo:");

        // Ряд из радиокнопок + Deselect. Кладём через SameLine, но переносим на новую
        // строку, когда следующий элемент не влезает в ширину панели (штатный паттерн
        // «manual wrapping» из imgui_demo: решаем по правому краю уже нарисованного).
        const ImGuiStyle& st = ImGui::GetStyle();
        const float square = ImGui::GetFrameHeight();                                  // диаметр кружка
        const float right_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        auto radio_w  = [&](const char* s) { return square + st.ItemInnerSpacing.x + ImGui::CalcTextSize(s).x; };
        auto button_w = [&](const char* s) { return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f; };
        auto keep_or_wrap = [&](float next_w) {                                         // перед следующим виджетом
            if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + next_w < right_x)
                ImGui::SameLine();                                                      // влезает — продолжаем строку
        };

        keep_or_wrap(radio_w("Move"));
        if (ImGui::RadioButton("Move",    g_gizmo_op == ImGuizmo::TRANSLATE)) g_gizmo_op = ImGuizmo::TRANSLATE;
        keep_or_wrap(radio_w("Rotate"));
        if (ImGui::RadioButton("Rotate",  g_gizmo_op == ImGuizmo::ROTATE))    g_gizmo_op = ImGuizmo::ROTATE;
        keep_or_wrap(radio_w("Scale"));
        if (ImGui::RadioButton("Scale",   g_gizmo_op == ImGuizmo::SCALE))     g_gizmo_op = ImGuizmo::SCALE;
        keep_or_wrap(radio_w("Uniform"));
        if (ImGui::RadioButton("Uniform", g_gizmo_op == ImGuizmo::SCALEU))    g_gizmo_op = ImGuizmo::SCALEU;
        // Ориентация осей ручек: World — по мировым осям, Local — по текущему повороту
        // объекта. Pivot в обоих случаях на объекте (см. DrawGizmo). На SCALE mode не влияет.
        keep_or_wrap(radio_w("World"));
        if (ImGui::RadioButton("World",   g_gizmo_mode == ImGuizmo::WORLD))   g_gizmo_mode = ImGuizmo::WORLD;
        keep_or_wrap(radio_w("Local"));
        if (ImGui::RadioButton("Local",   g_gizmo_mode == ImGuizmo::LOCAL))   g_gizmo_mode = ImGuizmo::LOCAL;
        keep_or_wrap(button_w("Deselect"));
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

    case SelKind::Pass:
        InspectPass(ctx, g_sel.name);
        break;

    case SelKind::Vsd: VsdEditor(ctx); break;
    case SelKind::Fsd: FsdEditor(ctx); break;
    case SelKind::Csd: CsdEditor(ctx); break;

    case SelKind::UINode:
    {
        // Узел дерева UI_Yoga: XY двигает гизмо, Z (слой) — кнопками (стрелка Z в упор не берётся).
        UI_Yoga* yg = ctx->GetUIYoga();
        if (!yg) { ImGui::TextDisabled("no UI tree"); break; }
        const UI_Yoga::Node n = g_sel.ui_node;
        ImGui::Text("UI node: %s", yg->NodeLabel(n).c_str());
        float dx, dy, dz; yg->GetOffset(n, dx, dy, dz);
        ImGui::Text("Offset: X=%.0f  Y=%.0f px", dx, dy);
        ImGui::Separator();
        auto nudge_z = [&](float ddz) {
            ctx->GetInputManager()->PushCommand(CommandId::NudgeUINode,
                new UINodeNudgeCmd{ n, 0.0f, 0.0f, ddz });
        };
        ImGui::Text("Z %.3f", dz);
        ImGui::SameLine();
        if (ImGui::SmallButton("-"))         nudge_z(+0.01f);   // дальше (больше z, под другими)
        ImGui::SameLine();
        if (ImGui::SmallButton("+"))         nudge_z(-0.01f);   // ближе (меньше z, поверх)
        ImGui::SameLine();
        if (ImGui::SmallButton("Deselect"))  g_sel = Selection{};
        break;
    }

    default:
        ImGui::TextDisabled("Nothing selected.");
        break;
    }

    ImGui::End();
}

void UI_ImGui::DrawGizmo(EngineContext* ctx)
{
    // UI-узел: свой гизмо. UI живёт в NDC (матрица = clip напрямую, без камеры) → скармливаем
    // ЕДИНИЧНЫЕ view/proj, иначе ImGuizmo прогнал бы NDC-позицию через мировую камеру и ручки
    // улетели бы в другое пространство. Только XY (TRANSLATE_X|Y): Z в упор не берётся, он в
    // инспекторе кнопками. Двигаем OFFSET узла (командой в sim), а не матрицу энтити.
    if (g_sel.kind == SelKind::UINode) {
        UI_Yoga* yg = ctx->GetUIYoga();
        if (!yg) return;
        const UI_Yoga::Node n = g_sel.ui_node;
        float cx, cy, z;
        if (!yg->GetNodeNdc(n, cx, cy, z)) return;   // узел ещё не эмитился

        ImGuiIO& io = ImGui::GetIO();
        glm::mat4 view(1.0f), proj(1.0f);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(cx, cy, z));

        ImGuizmo::SetOrthographic(true);
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

        const ImGuizmo::OPERATION op =
            static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
        float delta[16];
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                 op, ImGuizmo::WORLD, glm::value_ptr(model), delta))
        {
            const float dtx = delta[12], dty = delta[13];   // приращение переноса за кадр (в NDC)
            if (dtx != 0.0f || dty != 0.0f) {
                // NDC → px (Y флип: NDC вверх, layout-px вниз). Двигаем offset узла командой.
                ctx->GetInputManager()->PushCommand(CommandId::NudgeUINode,
                    new UINodeNudgeCmd{ n,  dtx * io.DisplaySize.x * 0.5f,
                                           -dty * io.DisplaySize.y * 0.5f, 0.0f });
            }
        }
        return;
    }

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
    const glm::mat4 model_before = model;   // до манипуляции — база для инверсии поворота

    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    // Перенос/скейл берём из родной матрицы ImGuizmo как есть (они верны, pivot на объекте).
    // Поворот у ImGuizmo идёт в ПРОТИВОПОЛОЖНУЮ сторону (несовпадение хендедности рендера с тем,
    // что ждёт ImGuizmo: translate ок, знак угла инвертирован). Инвертируем НАПРАВЛЕНИЕ, сохраняя
    // пивот и масштаб: мировой поворот кадра вокруг пивота = model_after·model_before⁻¹ (жёсткий
    // поворот — масштаб сокращается); берём его инверсию вокруг того же пивота →
    // model_before·inverse(model_after)·model_before. Никакого delta и двойного учёта пивота.
    if (ImGuizmo::Manipulate(glm::value_ptr(view),
                             glm::value_ptr(proj),
                             g_gizmo_op, g_gizmo_mode,
                             glm::value_ptr(model)))
    {
        if (g_gizmo_op == ImGuizmo::ROTATE)                 // model здесь = model_after от ImGuizmo
            model = model_before * glm::inverse(model) * model_before;

        SetTransformCmd* cmd = new SetTransformCmd{};
        cmd->entity = selected;
        std::memcpy(cmd->matrix, glm::value_ptr(model), sizeof(cmd->matrix));
        ctx->GetInputManager()->PushCommand(CommandId::SetTransform, cmd);
    }
}
