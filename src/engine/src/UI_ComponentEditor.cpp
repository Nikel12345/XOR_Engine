#include "PCH.h"
#include "UI_ComponentEditor.h"
#include "BaseComponents.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "InputCommands.h"
// EngineContext держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "ModelManager.h"
#include "MaterialManager.h"
#include <cstring>
#include <cstdio>

using namespace ui;

namespace {

// Подпись виджета группы: что объявила схема, иначе key первого поля.
const char* GroupLabel(const FieldSpec& f) { return f.group_label ? f.group_label : f.key; }

// Поле не редактируется. Либо сказано явно (.ReadOnly — сеттер есть, он нужен ЗАГРУЗКЕ, но
// UI писать не должен), либо сеттера нет вовсе — значит поле вычисляемое, писать некуда.
bool ReadOnly(const FieldSpec& f) { return f.ui_readonly || (!f.set_num && !f.set_str); }

} // namespace

bool ui::DrawComponentFields(const EditTarget& target, const ComponentSpec& spec,
                             Archetype& arch, size_t row)
{
    // Свой ID-скоуп: лейблы полей не конфликтуют ни с заголовком секции (CollapsingHeader
    // с именем компонента), ни между компонентами.
    ImGui::PushID(spec.name.c_str());
    bool edited = false;   // прямая запись в колонку → после цикла дёргаем after_edit
    bool sent   = false;   // ушло командой: колонку НЕ трогали, after_edit сделает хендлер
    const auto& fs = spec.fields;

    // Поле с командой у ЖИВОЙ энтити: правка уходит в sim ВМЕСТО записи в колонку. Обе ветки
    // сразу — иначе UI-поток всё равно писал бы живой ECS, ради чего команда и заводилась,
    // а хендлер записал бы второй раз. У черновика (kNoEntity) команде некому адресоваться.
    const bool routed = target.live();
    auto put_num = [&](const FieldSpec& f, double v) {
        if (routed && f.cmd != CommandId::None) {
            target.ctx->GetInputManager()->PushCommand(f.cmd,
                new FieldEditCmd{ target.entity, spec.name, f.key, v, {} });
            sent = true;
        }
        else { f.set_num(arch, row, v); edited = true; }
    };
    auto put_str = [&](const FieldSpec& f, std::string v) {
        if (routed && f.cmd != CommandId::None) {
            target.ctx->GetInputManager()->PushCommand(f.cmd,
                new FieldEditCmd{ target.entity, spec.name, f.key, 0.0, std::move(v) });
            sent = true;
        }
        else { f.set_str(arch, row, std::move(v)); edited = true; }
    };

    for (size_t i = 0; i < fs.size(); ) {
        // ---- группа: N подряд идущих полей одним виджетом (объявлена схемой, см. FieldGroup) ----
        // Диапазон/шаг берём у первого поля группы — оно её и открывает.
        const size_t gsize = FieldGroupSize(fs[i].group);
        if (gsize > 1 && i + gsize <= fs.size()) {
            const FieldSpec& f = fs[i];
            float v[16];
            for (size_t k = 0; k < gsize; ++k) v[k] = (float)fs[i + k].get_num(arch, row);
            bool changed = false;

            ImGui::BeginDisabled(ReadOnly(f));
            switch (f.group) {
            case FieldGroup::Color3:
                changed = ImGui::ColorEdit3(GroupLabel(f), v);
                break;
            case FieldGroup::Mat4: {
                // Строго в порядке объявления: 4 строки по 4 колонки. Подпись строки — её же
                // ключи, чтобы не выдумывать имён (у Positions это x y z w / a b c d / ...).
                for (int r = 0; r < 4; ++r) {
                    char label[64];
                    snprintf(label, sizeof(label), "%s %s %s %s",
                             fs[i + r * 4 + 0].key, fs[i + r * 4 + 1].key,
                             fs[i + r * 4 + 2].key, fs[i + r * 4 + 3].key);
                    changed |= ImGui::DragFloat4(label, v + r * 4, f.speed);
                }
                break;
            }
            default:   // Vec3
                changed = ImGui::DragFloat3(GroupLabel(f), v, f.speed, f.lo, f.hi, "%.3f",
                                            f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0);
                break;
            }
            ImGui::EndDisabled();

            if (changed) for (size_t k = 0; k < gsize; ++k) put_num(fs[i + k], v[k]);
            i += gsize;
            continue;
        }

        // ---- одиночное поле по kind ----
        // Нередактируемое (вычисляемое либо .ReadOnly) рисуется МЕТКОЙ, а не гашеным виджетом:
        // задизейбленный драг читается как «сломанная крутилка», а не как «это расчёт».
        const FieldSpec& f = fs[i];
        const bool ro = ReadOnly(f);
        switch (f.kind) {
        case FieldKind::F32: {
            float v = (float)f.get_num(arch, row);
            if (ro) { ImGui::LabelText(f.key, "%.3f", v); break; }
            if (ImGui::DragFloat(f.key, &v, f.speed, f.lo, f.hi, "%.3f",
                                 f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                put_num(f, v);
            }
            break;
        }
        case FieldKind::Angle: {   // радианы в данных, слайдер в градусах (lo/hi схемы — градусы)
            float v = (float)f.get_num(arch, row);
            if (ro) { ImGui::LabelText(f.key, "%.1f deg", v * 57.2957795f); break; }
            const bool ranged = f.lo < f.hi;
            if (ImGui::SliderAngle(f.key, &v, ranged ? f.lo : -360.0f, ranged ? f.hi : 360.0f)) {
                put_num(f, v);
            }
            break;
        }
        case FieldKind::U32: {
            int v = (int)f.get_num(arch, row);
            if (ro) { ImGui::LabelText(f.key, "%d", v); break; }
            if (ImGui::DragInt(f.key, &v, f.speed < 1.0f ? 1.0f : f.speed, (int)f.lo, (int)f.hi, "%d",
                               f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                if (v < 0) v = 0;
                put_num(f, v);
            }
            break;
        }
        case FieldKind::Bool: {
            bool v = f.get_num(arch, row) != 0.0;
            if (ro) { ImGui::LabelText(f.key, "%s", v ? "true" : "false"); break; }
            if (ImGui::Checkbox(f.key, &v)) put_num(f, v ? 1.0 : 0.0);
            break;
        }
        case FieldKind::AssetModel: {   // имя ассета — комбо из менеджера, а не ввод строки
            const std::string& sel = f.get_str(arch, row);
            if (ro || !target.ctx) { ImGui::LabelText(f.key, "%s", sel.c_str()); break; }
            if (ImGui::BeginCombo(f.key, sel.empty() ? "(none)" : sel.c_str())) {
                if (ImGui::Selectable("(none)", sel.empty())) put_str(f, {});
                for (auto& [nm, m] : target.ctx->GetModelManager()->GetModels()) {
                    if (!g_show_internal && IsInternalName(nm)) continue;
                    if (ImGui::Selectable(nm.c_str(), nm == sel)) put_str(f, nm);
                }
                ImGui::EndCombo();
            }
            break;
        }
        default: {   // Str
            if (ro) { ImGui::LabelText(f.key, "%s", f.get_str(arch, row).c_str()); break; }
            char buf[256];
            snprintf(buf, sizeof buf, "%s", f.get_str(arch, row).c_str());
            if (ImGui::InputText(f.key, buf, sizeof buf)) put_str(f, buf);
            break;
        }
        }
        ++i;
    }

    ImGui::PopID();
    if (edited && spec.after_edit) spec.after_edit(arch, row);
    return edited || sent;
}

bool ui::DrawParamsFields(const ParamsSpec& spec, std::vector<uint8_t>& blob)
{
    ImGui::PushID(spec.name.c_str());
    bool edited = false;

    for (const ParamsFieldSpec& f : spec.fields) {
        // Поле не влезает в блоб — блоб старше/младше схемы. Молча не рисуем: писать по этому
        // смещению значило бы портить чужую память (реестр такие поля отбраковывает на
        // регистрации, сюда доходит только рассинхрон размера самого блоба).
        void* p = ParamsFieldPtr(blob, f);
        if (!p) continue;

        const bool  ranged = f.lo < f.hi;
        const auto  flags  = ranged ? ImGuiSliderFlags_AlwaysClamp : 0;
        const char* label  = f.UiLabel();
        ImGui::BeginDisabled(f.ui_readonly);
        switch (f.kind) {
        case ParamsFieldKind::Color3:
            edited |= ImGui::ColorEdit3(label, static_cast<float*>(p));
            break;
        case ParamsFieldKind::Color4:
            edited |= ImGui::ColorEdit4(label, static_cast<float*>(p));
            break;
        case ParamsFieldKind::Vec2:
            edited |= ImGui::DragFloat2(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case ParamsFieldKind::Vec3:
            edited |= ImGui::DragFloat3(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case ParamsFieldKind::Vec4:
            edited |= ImGui::DragFloat4(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case ParamsFieldKind::Angle: {   // радианы в блобе, слайдер в градусах (lo/hi схемы — градусы)
            edited |= ImGui::SliderAngle(label, static_cast<float*>(p),
                                         ranged ? f.lo : -360.0f, ranged ? f.hi : 360.0f);
            break;
        }
        case ParamsFieldKind::U32: {
            auto* u = static_cast<uint32_t*>(p);
            int v = static_cast<int>(*u);
            if (ImGui::DragInt(label, &v, f.speed < 1.0f ? 1.0f : f.speed, (int)f.lo, (int)f.hi, "%d", flags)) {
                *u = static_cast<uint32_t>(v < 0 ? 0 : v);
                edited = true;
            }
            break;
        }
        case ParamsFieldKind::Bool: {   // в cbuffer bool — 4 байта, на CPU держим uint32_t
            auto* u = static_cast<uint32_t*>(p);
            bool v = (*u != 0);
            if (ImGui::Checkbox(label, &v)) { *u = v ? 1u : 0u; edited = true; }
            break;
        }
        default:   // F32: слайдер при заданном диапазоне, иначе драг
            if (ranged) edited |= ImGui::SliderFloat(label, static_cast<float*>(p), f.lo, f.hi);
            else        edited |= ImGui::DragFloat(label, static_cast<float*>(p), f.speed);
            break;
        }
        ImGui::EndDisabled();
    }

    ImGui::PopID();
    return edited;
}

// ─────────────────────────────────────────────────────────────────────────────────────────
//  Секции, которые схемой не выражаются, и сборка вида целиком.
// ─────────────────────────────────────────────────────────────────────────────────────────
namespace {

// Material: зубчатый список имён. Схемой (FieldSpec — ФИКСИРОВАННЫЙ набор полей) он не
// выражается — ровно поэтому в реестре у него custom_save/custom_load вместо fields. Здесь
// зеркало того же escape hatch со стороны UI, и адресуется оно так же — по факту custom_save,
// а не по имени компонента.
//
// Слоты диктует модель (как required_slots шейдера диктует слот-роли текстур у материала):
// слот = сабмеш, его material_index адресует этот список, поэтому длина всегда равна числу
// сабмешей — добавить/убрать нечего. Модель не выбрана или не найдена → список пуст.
// У живой энтити длину НЕ правим: её приводит хендлер SetEntityModel (там же и QueueUpdate).
void DrawMaterialSection(const EditTarget& t, Archetype& arch, size_t row)
{
    MaterialComponent& mats = (*arch.get_array<MaterialComponent>())[row];

    if (!t.live()) {
        size_t sub_count = 0;
        if (auto* mdl_arr = arch.get_array<ModelComponent>()) {
            const auto& models = t.ctx->GetModelManager()->GetModels();
            auto mit = models.find((*mdl_arr)[row].name);
            if (mit != models.end()) sub_count = mit->second->submeshes.size();
        }
        if (mats.names.size() != sub_count) mats.names.resize(sub_count);
    }

    for (size_t k = 0; k < mats.names.size(); ++k) {
        const std::string sel = mats.names[k];   // копия: правка живой энтити идёт командой
        char label[32];
        snprintf(label, sizeof(label), "submesh %zu", k);

        if (!ImGui::BeginCombo(label, sel.empty() ? "(none)" : sel.c_str())) continue;
        for (auto& [nm, m] : t.ctx->GetMaterialManager()->GetMaterials()) {
            if (!g_show_internal && IsInternalName(nm)) continue;
            if (!ImGui::Selectable(nm.c_str(), nm == sel)) continue;
            if (t.live())
                t.ctx->GetInputManager()->PushCommand(CommandId::SetEntityMaterial,
                    new FieldEditCmd{ t.entity, "Material", "names", (double)k, nm });
            else
                mats.names[k] = nm;
        }
        ImGui::EndCombo();
    }
}

} // namespace

void ui::DrawEntityComponents(const EditTarget& target, Archetype& arch, size_t row)
{
    std::string tags;   // теги без данных — одной строкой внизу, не секциями
    for (const ComponentSpec& s : ComponentSpecRegistry::Get().All()) {
        if (!arch.components.count(s.sig_type)) continue;
        if (s.fields.empty() && !s.custom_save) {
            tags += tags.empty() ? s.name : ", " + s.name;
            continue;
        }
        if (!ImGui::CollapsingHeader(s.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

        // custom_save = «схемой не выражается» (сейчас только Material) → рисует своя функция,
        // как и сохраняет. Всё остальное — generic по fields, без исключений по именам.
        if (s.custom_save) DrawMaterialSection(target, arch, row);
        else               DrawComponentFields(target, s, arch, row);
    }
    if (!tags.empty()) { ImGui::Separator(); ImGui::Text("Tags: %s", tags.c_str()); }
}
