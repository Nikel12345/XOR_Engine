#include "PCH.h"
#include "UI_Widgets.h"

namespace ui {

namespace {
    // Акцент-цвет по типу ассета (рамка/фон/глиф затычки).
    ImU32 IconAccent(AssetIcon k)
    {
        switch (k) {
        case AssetIcon::Texture:  return IM_COL32( 80, 190, 190, 255);   // бирюза
        case AssetIcon::Model:    return IM_COL32(220, 140,  65, 255);   // оранж
        case AssetIcon::Material: return IM_COL32(165, 120, 215, 255);   // фиолет
        case AssetIcon::Shader:   return IM_COL32(115, 190, 105, 255);   // зелень
        case AssetIcon::Compute:  return IM_COL32(215, 105, 105, 255);   // красн
        case AssetIcon::Vsd:      return IM_COL32(105, 155, 230, 255);   // синь
        case AssetIcon::Fsd:      return IM_COL32(215, 190,  80, 255);   // жёлт
        case AssetIcon::Csd:      return IM_COL32(215, 115, 165, 255);   // роз
        default:                  return IM_COL32(150, 150, 150, 255);
        }
    }
    const char* IconTag(AssetIcon k)
    {
        switch (k) {
        case AssetIcon::Shader:  return "SP";
        case AssetIcon::Compute: return "CSP";
        case AssetIcon::Vsd:     return "VS";
        case AssetIcon::Fsd:     return "FS";
        case AssetIcon::Csd:     return "CS";
        default:                 return nullptr;   // у текстур/моделей/материалов — рисунок, не тег
        }
    }

    // Рисунок-затычка внутри квадрата плитки [p0, p0+size]: рамка акцент-цвета + фон-подложка +
    // глиф по типу (текстура — «фото», модель — куб, материал — сфера, шейдеры — тег-буквы).
    void DrawAssetIcon(ImDrawList* dl, ImVec2 p0, float size, AssetIcon k)
    {
        const ImU32 acc = IconAccent(k);
        const ImU32 bg  = (acc & 0x00FFFFFF) | 0x22000000;   // тот же тон, слабая заливка
        const float pad = size * 0.12f;
        const ImVec2 a{ p0.x + pad,        p0.y + pad };
        const ImVec2 b{ p0.x + size - pad, p0.y + size - pad };
        const float  w = b.x - a.x, h = b.y - a.y;
        const float  rnd = size * 0.07f;
        dl->AddRectFilled(a, b, bg, rnd);
        dl->AddRect(a, b, acc, rnd, 0, size * 0.028f);

        switch (k) {
        case AssetIcon::Texture: {   // «фото»: солнце + горы
            dl->AddCircleFilled(ImVec2(a.x + w * 0.30f, a.y + h * 0.30f), size * 0.06f, acc);
            ImVec2 m0{ a.x + w * 0.12f, b.y - h * 0.14f };
            ImVec2 m1{ a.x + w * 0.45f, a.y + h * 0.52f };
            ImVec2 m2{ b.x - w * 0.12f, b.y - h * 0.14f };
            dl->AddTriangleFilled(m0, m1, m2, acc);
            break;
        }
        case AssetIcon::Model: {     // куб: передняя грань + верх/бок (2.5D)
            const float s = w * 0.42f, ox = a.x + w * 0.30f, oy = a.y + h * 0.42f, d = s * 0.42f;
            dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + s, oy + s), bg);
            dl->AddRect(ImVec2(ox, oy), ImVec2(ox + s, oy + s), acc, 0, 0, size * 0.02f);
            dl->AddQuad(ImVec2(ox, oy), ImVec2(ox + d, oy - d), ImVec2(ox + s + d, oy - d), ImVec2(ox + s, oy), acc, size * 0.02f);   // верх
            dl->AddQuad(ImVec2(ox + s, oy), ImVec2(ox + s + d, oy - d), ImVec2(ox + s + d, oy + s - d), ImVec2(ox + s, oy + s), acc, size * 0.02f); // бок
            break;
        }
        case AssetIcon::Material: {  // сфера: круг + блик
            const ImVec2 c{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
            const float r = w * 0.30f;
            dl->AddCircleFilled(c, r, bg);
            dl->AddCircle(c, r, acc, 0, size * 0.028f);
            dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.28f, acc);   // блик
            break;
        }
        default: {                   // шейдеры: тег-буквы по центру
            if (const char* tag = IconTag(k)) {
                const ImVec2 ts = ImGui::CalcTextSize(tag);
                dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), acc, tag);
            }
            break;
        }
        }
    }
} // namespace

bool AssetTile(const char* name, bool selected, float size, AssetIcon icon)
{
    ImGui::BeginGroup();
    ImGui::PushID(name);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##sq", selected, ImGuiSelectableFlags_None, ImVec2(size, size));
    DrawAssetIcon(ImGui::GetWindowDrawList(), p0, size, icon);   // затычка поверх (Selectable рисует подсветку под)
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size);       // подпись не шире плитки
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
