#include "PCH.h"
#include "UI_Widgets.h"

namespace ui {

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

bool PlusTile(float size)
{
    ImGui::BeginGroup();
    ImGui::PushID("##new");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button("##new", ImVec2(size, size));
    // «+» рисуем сами почти во весь квадрат (кнопочный текст "+" был бы крохотным).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = p0.x + size * 0.5f, cy = p0.y + size * 0.5f;
    const float arm = size * 0.35f, th = size * 0.076f;   // толщина на ~5% меньше
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddRectFilled(ImVec2(cx - th, cy - arm), ImVec2(cx + th, cy + arm), col);   // вертикаль
    dl->AddRectFilled(ImVec2(cx - arm, cy - th), ImVec2(cx + arm, cy + th), col);   // горизонталь
    ImGui::TextUnformatted("New");
    ImGui::PopID();
    ImGui::EndGroup();
    return clicked;
}

bool DangerButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

} // namespace ui
