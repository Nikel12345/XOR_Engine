#include "PCH.h"
#include "UI_ComponentEditor.h"
#include <cstring>
#include <cstdio>

using namespace ui;

namespace {

// Поле пригодно к числовой группировке (тройки рисуются одним виджетом).
bool GroupableF32(const FieldSpec& f)
{
    return f.kind == FieldKind::F32 && f.get_num && f.set_num;
}

// Тройка ключей "<p>_x","<p>_y","<p>_z" (label=p) либо "x","y","z" (label=fallback).
bool TripleKeys(const char* kx, const char* ky, const char* kz, const char* fallback,
                char* label, size_t label_sz)
{
    if (!strcmp(kx, "x") && !strcmp(ky, "y") && !strcmp(kz, "z")) {
        snprintf(label, label_sz, "%s", fallback);
        return true;
    }
    const size_t n = strlen(kx);
    if (n < 3 || strcmp(kx + n - 2, "_x")) return false;
    if (strlen(ky) != n || strlen(kz) != n) return false;
    if (strncmp(kx, ky, n - 1) || strncmp(kx, kz, n - 1)) return false;
    if (ky[n - 1] != 'y' || kz[n - 1] != 'z') return false;
    snprintf(label, label_sz, "%.*s", (int)(n - 2), kx);
    return true;
}

} // namespace

bool ui::DrawComponentFields(const ComponentSpec& spec, Archetype& arch, size_t row)
{
    // Свой ID-скоуп: лейблы полей/троек не конфликтуют ни с заголовком секции (CollapsingHeader
    // с именем компонента — напр. тройка x,y,z получает лейбл spec.name), ни между компонентами.
    ImGui::PushID(spec.name.c_str());
    bool edited = false;
    const auto& fs = spec.fields;

    for (size_t i = 0; i < fs.size(); ) {
        // ---- группировки-конвенции (только соседние F32) ----
        if (i + 2 < fs.size() && GroupableF32(fs[i]) && GroupableF32(fs[i + 1]) && GroupableF32(fs[i + 2])) {
            // r,g,b → ColorEdit3
            if (!strcmp(fs[i].key, "r") && !strcmp(fs[i + 1].key, "g") && !strcmp(fs[i + 2].key, "b")) {
                float rgb[3] = { (float)fs[i].get_num(arch, row), (float)fs[i + 1].get_num(arch, row),
                                 (float)fs[i + 2].get_num(arch, row) };
                ImGui::BeginDisabled(fs[i].ui_readonly);
                if (ImGui::ColorEdit3("RGB", rgb)) {
                    for (int k = 0; k < 3; ++k) fs[i + k].set_num(arch, row, rgb[k]);
                    edited = true;
                }
                ImGui::EndDisabled();
                i += 3; continue;
            }
            // <p>_x,_y,_z / x,y,z → DragFloat3 (диапазон/шаг — от первого поля тройки)
            char label[64];
            if (TripleKeys(fs[i].key, fs[i + 1].key, fs[i + 2].key, spec.name.c_str(), label, sizeof label)) {
                const FieldSpec& f = fs[i];
                float v[3] = { (float)fs[i].get_num(arch, row), (float)fs[i + 1].get_num(arch, row),
                               (float)fs[i + 2].get_num(arch, row) };
                ImGui::BeginDisabled(f.ui_readonly);
                if (ImGui::DragFloat3(label, v, f.speed, f.lo, f.hi, "%.3f",
                                      f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                    for (int k = 0; k < 3; ++k) fs[i + k].set_num(arch, row, v[k]);
                    edited = true;
                }
                ImGui::EndDisabled();
                i += 3; continue;
            }
        }

        // ---- одиночное поле по kind ----
        const FieldSpec& f = fs[i];
        if (f.ui_hidden) { ++i; continue; }   // у поля свой контрол в UI-слое (см. ui_hidden)
        ImGui::BeginDisabled(f.ui_readonly);
        switch (f.kind) {
        case FieldKind::F32: {
            float v = (float)f.get_num(arch, row);
            if (ImGui::DragFloat(f.key, &v, f.speed, f.lo, f.hi, "%.3f",
                                 f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                f.set_num(arch, row, v); edited = true;
            }
            break;
        }
        case FieldKind::Angle: {   // радианы в данных, слайдер в градусах (lo/hi схемы — градусы)
            float v = (float)f.get_num(arch, row);
            const bool ranged = f.lo < f.hi;
            if (ImGui::SliderAngle(f.key, &v, ranged ? f.lo : -360.0f, ranged ? f.hi : 360.0f)) {
                f.set_num(arch, row, v); edited = true;
            }
            break;
        }
        case FieldKind::U32: {
            int v = (int)f.get_num(arch, row);
            if (ImGui::DragInt(f.key, &v, f.speed < 1.0f ? 1.0f : f.speed, (int)f.lo, (int)f.hi, "%d",
                               f.lo < f.hi ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                if (v < 0) v = 0;
                f.set_num(arch, row, v); edited = true;
            }
            break;
        }
        case FieldKind::Bool: {
            bool v = f.get_num(arch, row) != 0.0;
            if (ImGui::Checkbox(f.key, &v)) { f.set_num(arch, row, v ? 1.0 : 0.0); edited = true; }
            break;
        }
        default: {   // Str / Asset*: readonly → метка; иначе прямое редактирование имени
            if (f.ui_readonly) {
                ImGui::LabelText(f.key, "%s", f.get_str(arch, row).c_str());
            }
            else {
                char buf[256];
                snprintf(buf, sizeof buf, "%s", f.get_str(arch, row).c_str());
                if (ImGui::InputText(f.key, buf, sizeof buf)) { f.set_str(arch, row, buf); edited = true; }
            }
            break;
        }
        }
        ImGui::EndDisabled();
        ++i;
    }

    ImGui::PopID();
    if (edited && spec.after_edit) spec.after_edit(arch, row);
    return edited;
}

bool ui::DrawMaterialParamsFields(const MaterialParamsSpec& spec, std::vector<uint8_t>& blob)
{
    ImGui::PushID(spec.name.c_str());
    bool edited = false;

    for (const MatFieldSpec& f : spec.fields) {
        // Поле не влезает в блоб — блоб старше/младше схемы. Молча не рисуем: писать по этому
        // смещению значило бы портить чужую память (реестр такие поля отбраковывает на
        // регистрации, сюда доходит только рассинхрон размера самого блоба).
        void* p = MatFieldPtr(blob, f);
        if (!p) continue;

        const bool  ranged = f.lo < f.hi;
        const auto  flags  = ranged ? ImGuiSliderFlags_AlwaysClamp : 0;
        const char* label  = f.UiLabel();
        ImGui::BeginDisabled(f.ui_readonly);
        switch (f.kind) {
        case MatFieldKind::Color3:
            edited |= ImGui::ColorEdit3(label, static_cast<float*>(p));
            break;
        case MatFieldKind::Color4:
            edited |= ImGui::ColorEdit4(label, static_cast<float*>(p));
            break;
        case MatFieldKind::Vec2:
            edited |= ImGui::DragFloat2(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case MatFieldKind::Vec3:
            edited |= ImGui::DragFloat3(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case MatFieldKind::Vec4:
            edited |= ImGui::DragFloat4(label, static_cast<float*>(p), f.speed, f.lo, f.hi, "%.3f", flags);
            break;
        case MatFieldKind::Angle: {   // радианы в блобе, слайдер в градусах (lo/hi схемы — градусы)
            edited |= ImGui::SliderAngle(label, static_cast<float*>(p),
                                         ranged ? f.lo : -360.0f, ranged ? f.hi : 360.0f);
            break;
        }
        case MatFieldKind::U32: {
            auto* u = static_cast<uint32_t*>(p);
            int v = static_cast<int>(*u);
            if (ImGui::DragInt(label, &v, f.speed < 1.0f ? 1.0f : f.speed, (int)f.lo, (int)f.hi, "%d", flags)) {
                *u = static_cast<uint32_t>(v < 0 ? 0 : v);
                edited = true;
            }
            break;
        }
        case MatFieldKind::Bool: {   // в cbuffer bool — 4 байта, на CPU держим uint32_t
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
