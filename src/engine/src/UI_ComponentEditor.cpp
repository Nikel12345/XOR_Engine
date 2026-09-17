#include "PCH.h"
#include "UI_ComponentEditor.h"
#include "BaseComponents.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "InputCommands.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "MaterialData.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

using namespace ui;

namespace {

const char* GroupLabel(const FieldSpec& f) { return f.group_label ? f.group_label : f.key; }

bool ReadOnly(const FieldSpec& f) { return f.ui_readonly || (!f.set_num && !f.set_str); }

}

bool ui::DrawComponentFields(const EditTarget& target, const ComponentSpec& spec,
                             Archetype& arch, size_t row)
{
    // Свой ID-скоуп: иначе одноимённые поля разных компонентов схлопнутся в один виджет.
    ImGui::PushID(spec.name.c_str());
    bool edited = false;   // прямая запись в колонку → после цикла дёргаем after_edit
    bool sent   = false;   // ушло командой: колонку НЕ трогали, after_edit сделает хендлер
    const auto& fs = spec.fields;

    // Команда ВМЕСТО записи в колонку, а не вместе с ней: иначе UI-поток всё равно писал бы
    // живой ECS, ради чего команда и заводилась, а хендлер записал бы второй раз.
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
                for (int r = 0; r < 4; ++r) {
                    char label[64];
                    snprintf(label, sizeof(label), "%s %s %s %s",
                             fs[i + r * 4 + 0].key, fs[i + r * 4 + 1].key,
                             fs[i + r * 4 + 2].key, fs[i + r * 4 + 3].key);
                    changed |= ImGui::DragFloat4(label, v + r * 4, f.speed);
                }
                break;
            }
            default:
                changed = ImGui::DragFloat3(GroupLabel(f), v, f.speed, f.lo, f.hi, "%.3f",
                                            f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0);
                break;
            }
            ImGui::EndDisabled();

            if (changed) for (size_t k = 0; k < gsize; ++k) put_num(fs[i + k], v[k]);
            i += gsize;
            continue;
        }

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
        case FieldKind::Angle: {
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
        case FieldKind::AssetModel: {
            const std::string& sel = f.get_str(arch, row);
            if (ro || !target.ctx) { ImGui::LabelText(f.key, "%s", sel.c_str()); break; }
            if (ImGui::BeginCombo(f.key, sel.empty() ? "(none)" : sel.c_str())) {
                if (ImGui::Selectable("(none)", sel.empty())) put_str(f, {});
                const ModelRegistry& mdreg = target.ctx->GetModelManager()->Models();
                for (int32_t mdi = 0; mdi < mdreg.Count(); ++mdi) {
                    const std::string& nm = mdreg.At(mdi).name;
                    const auto& m = mdreg.At(mdi).object;
                    if (!m) continue;
                    if (m && !g_show_internal && HasTag(m->tags, ResourceTag::System)) continue;
                    if (ImGui::Selectable(nm.c_str(), nm == sel)) put_str(f, nm);
                }
                ImGui::EndCombo();
            }
            break;
        }
        default: {
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
        case ParamsFieldKind::Angle: {
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
        case ParamsFieldKind::Bool: {
            auto* u = static_cast<uint32_t*>(p);
            bool v = (*u != 0);
            if (ImGui::Checkbox(label, &v)) { *u = v ? 1u : 0u; edited = true; }
            break;
        }
        default:
            if (ranged) edited |= ImGui::SliderFloat(label, static_cast<float*>(p), f.lo, f.hi);
            else        edited |= ImGui::DragFloat(label, static_cast<float*>(p), f.speed);
            break;
        }
        ImGui::EndDisabled();
    }

    ImGui::PopID();
    return edited;
}

namespace {

void DrawMaterialSection(const EditTarget& t, Archetype& arch, size_t row)
{
    MaterialComponent& mats = (*arch.get_array<MaterialComponent>())[row];

    if (!t.live()) {
        size_t sub_count = 0;
        if (auto* mdl_arr = arch.get_array<ModelComponent>()) {
            if (const ModelData* m = t.ctx->GetModelManager()->FindModel((*mdl_arr)[row].model))
                sub_count = m->submeshes.size();
        }
        if (mats.materials.size() != sub_count) mats.materials.resize(sub_count);
    }

    for (size_t k = 0; k < mats.materials.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        MaterialManager* mmgr = t.ctx->GetMaterialManager();
        const MaterialId sel_id = mats.materials[k].material;
        const std::string sel = mmgr->MaterialNameOf(sel_id);
        char label[32];
        snprintf(label, sizeof(label), "submesh %zu", k);

        if (ImGui::BeginCombo(label, sel.empty() ? "(none)" : sel.c_str())) {
            const MaterialRegistry& mreg = mmgr->Materials();
            for (int32_t mi = 0; mi < mreg.Count(); ++mi) {
                const MaterialCell& mc = mreg.At(mi);
                if (!mc.object) continue;
                if (!g_show_internal && HasTag(mc.object->tags, ResourceTag::System)) continue;
                if (!ImGui::Selectable(mc.name.c_str(), mc.name == sel)) continue;
                if (t.live())
                    t.ctx->GetInputManager()->PushCommand(CommandId::SetEntityMaterial,
                        new FieldEditCmd{ t.entity, "Material", "names", (double)k, mc.name });
                else
                    mats.materials[k] = MaterialRef{ MaterialId{ mi }, {} };
            }
            ImGui::EndCombo();
        }

        const Material* mat = mmgr->GetMaterial(sel_id);
        if (mat) {
            const VariativeRoles vr = CollectVariativeRoles(*mat);
            for (uint32_t c = 0; c < vr.count; ++c) {
                const TextureSlotRole role = vr.role[c];
                const uint32_t count = safe_u32(mat->textures.at(role).size());

                auto& st = mats.materials[k].states;
                auto sit = std::find_if(st.begin(), st.end(),
                    [role](const auto& pr) { return pr.first == role; });
                uint32_t cur = (sit != st.end() && sit->second < count) ? sit->second : 0u;

                ImGui::PushID(static_cast<int>(role));
                ImGui::TextUnformatted(RoleName(role));
                ImGui::SameLine();
                uint32_t next = cur;
                ImGui::BeginDisabled(cur == 0);
                if (ImGui::ArrowButton("prev", ImGuiDir_Left)) --next;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Text("%u / %u", cur, count - 1);
                ImGui::SameLine();
                ImGui::BeginDisabled(cur + 1 >= count);
                if (ImGui::ArrowButton("next", ImGuiDir_Right)) ++next;
                ImGui::EndDisabled();
                ImGui::PopID();

                if (next == cur) continue;
                if (t.live())
                    t.ctx->GetInputManager()->PushCommand(CommandId::SetEntityTextureVariant,
                        new EntityTextureVariantCmd{ t.entity, safe_u32(k),
                                                     static_cast<uint32_t>(role), next });
                else if (next == 0) { if (sit != st.end()) st.erase(sit); }
                else if (sit != st.end()) sit->second = next;
                else st.emplace_back(role, next);
            }
        }
        ImGui::PopID();
    }
}

}

void ui::DrawEntityComponents(const EditTarget& target, Archetype& arch, size_t row)
{
    std::string tags;
    for (const ComponentSpec& s : ComponentSpecRegistry::Get().All()) {
        if (!arch.components.count(s.sig_type)) continue;
        if (s.fields.empty() && !s.custom_save) {
            tags += tags.empty() ? s.name : ", " + s.name;
            continue;
        }
        if (!ImGui::CollapsingHeader(s.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

        if (s.custom_save) DrawMaterialSection(target, arch, row);
        else               DrawComponentFields(target, s, arch, row);
    }
    if (!tags.empty()) { ImGui::Separator(); ImGui::Text("Tags: %s", tags.c_str()); }
}
