#include "Views.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstdio>

namespace {

constexpr int kMaxGridColumns = IMGUI_TABLE_MAX_COLUMNS - 1;   // первая колонка — номер объекта

const ImVec4 kGrey = { 0.55f, 0.55f, 0.55f, 1.0f };
const ImVec4 kRed  = { 0.95f, 0.40f, 0.35f, 1.0f };

std::string ColumnTitle(const ViewTable& t, const ViewColumn& vc)
{
    return t.table->components[vc.component].name + "." + vc.col->name;
}

bool IsDefault(const ViewColumn& vc, uint32_t row)
{
    const sheaf::Column& c = *vc.col;
    if (c.flags & sheaf::List) return false;
    if ((c.flags & sheaf::Nullable) && !c.present[row]) return true;
    return c.values[row] == c.def;
}

bool IsAbsent(const sheaf::Column& c, size_t i) { return (c.flags & sheaf::Nullable) && !c.present[i]; }

// Selectable с текстом из файла: «##» в подписи ImGui считает началом id и не рисует остаток.
bool CellSelectable(const std::string& text, bool selected)
{
    if (text.find("##") == std::string::npos) return ImGui::Selectable(text.c_str(), selected);
    const char* b = text.data();
    const char* e = b + text.size();
    const bool clicked = ImGui::Selectable("##cell", selected, 0, { ImGui::CalcTextSize(b, e).x, 0.0f });
    ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), ImGui::GetColorU32(ImGuiCol_Text), b, e);
    return clicked;
}

void SyncFilterUi(ViewerState& s)
{
    const ViewTable& t = s.model.Tables()[s.current];
    s.filter_column = t.filter.column;
    std::snprintf(s.filter_text, sizeof s.filter_text, "%s", t.filter.text.c_str());
    s.filter_bad = false;
}

void SelectTable(ViewerState& s, int index)
{
    if (s.current == index) return;
    s.current = index;
    s.sel     = { index, 0, -1 };
    SyncFilterUi(s);
}

void Navigate(ViewerState& s, uint32_t global)
{
    const auto loc = s.model.Locate(global);
    if (!loc) { s.error = "объекта " + std::to_string(global) + " в файле нет"; return; }
    SelectTable(s, sheaf::Narrow<int>(loc->first));
    ViewTable& t = s.model.Tables()[loc->first];
    auto it = std::find(t.view.begin(), t.view.end(), loc->second);
    if (it == t.view.end()) {
        t.filter = {};
        s.model.Rebuild(t);
        SyncFilterUi(s);
        it = std::find(t.view.begin(), t.view.end(), loc->second);
    }
    s.sel       = { s.current, loc->second, -1 };
    s.scroll_to = sheaf::Narrow<int>(it - t.view.begin());
}

// Значение одной ячейки (элемента списка) в панели выбранного: ссылка — переходом.
void DrawValue(ViewerState& s, const sheaf::Column& c, size_t i)
{
    if (IsAbsent(c, i)) { ImGui::TextColored(kGrey, "—"); return; }
    const std::string text = s.model.FormatValue(c, c.values[i]);
    if (c.type != sheaf::Type::Ref) { ImGui::TextUnformatted(text.c_str()); return; }
    ImGui::PushID(sheaf::Narrow<int>(i));
    if (ImGui::TextLink(("→ " + text).c_str())) s.navigate_to = c.values[i];
    ImGui::PopID();
}

void DrawTableList(ViewerState& s)
{
    std::vector<ViewTable>& tables = s.model.Tables();
    for (int i = 0; i < sheaf::Narrow<int>(tables.size()); ++i) {
        const sheaf::Table& t = *tables[i].table;
        std::string label;
        for (const sheaf::Component& c : t.components) label += (label.empty() ? "" : "\n") + c.name;
        if (label.empty()) label = "(без компонентов)";
        ImGui::PushID(i);
        if (ImGui::Selectable(label.c_str(), s.current == i)) SelectTable(s, i);
        const std::string rows = std::to_string(t.rows);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText({ max.x - ImGui::CalcTextSize(rows.c_str()).x, min.y },
                                            ImGui::GetColorU32(kGrey), rows.c_str());
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void DrawToolbar(ViewerState& s, ViewTable& t)
{
    std::string tags;
    for (uint32_t ci = 0; ci < t.table->components.size(); ++ci) {
        const sheaf::Component& comp = t.table->components[ci];
        if (comp.fields.empty()) { tags += (tags.empty() ? "" : ", ") + comp.name; continue; }
        bool shown = !t.hidden[ci];
        ImGui::PushID(sheaf::Narrow<int>(ci));
        if (ImGui::Checkbox(comp.name.c_str(), &shown)) t.hidden[ci] = !shown;
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (!tags.empty()) ImGui::TextColored(kGrey, "теги: %s", tags.c_str());
    else               ImGui::NewLine();

    ImGui::SetNextItemWidth(220);
    const std::string preview = s.filter_column >= 0 ? ColumnTitle(t, t.columns[s.filter_column]) : "фильтр: поле";
    if (ImGui::BeginCombo("##fcol", preview.c_str())) {
        if (ImGui::Selectable("(без фильтра)", s.filter_column < 0)) s.filter_column = -1;
        for (int i = 0; i < sheaf::Narrow<int>(t.columns.size()); ++i)
            if (ImGui::Selectable(ColumnTitle(t, t.columns[i]).c_str(), s.filter_column == i)) s.filter_column = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("=");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    bool apply = ImGui::InputTextWithHint("##ftext", "значение; — пусто, \"\" пустая строка", s.filter_text,
                                          sizeof s.filter_text, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    apply |= ImGui::Button("Применить");
    ImGui::SameLine();
    if (ImGui::Button("Сбросить")) {
        s.filter_column  = -1;
        s.filter_text[0] = 0;
        apply = true;
    }
    if (apply) {
        const Filter old = t.filter;
        t.filter = { s.filter_column, s.filter_column >= 0 ? s.filter_text : "" };
        s.filter_bad = !s.model.Rebuild(t);
        if (s.filter_bad) t.filter = old;
    }
    if (s.filter_bad) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "не разобрать как %s", TypeName(t.columns[s.filter_column].col->type));
    }
}

// Строка с именами компонентов над именами полей. ImGui группы колонок не умеет: имя рисуется поверх
// всех видимых колонок компонента своим клипом, иначе его обрезало бы по ширине первой колонки.
void DrawComponentHeaderRow(const ViewTable& t, int ncols)
{
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGuiTable* table = ImGui::GetCurrentTable();
    const float row_y = table->RowPosY1;
    const float row_h = ImGui::GetTextLineHeight() + 2 * table->RowCellPaddingY;
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy({ 0.0f, ImGui::GetTextLineHeight() });   // имена рисуются мимо layout — высоту строке задаёт это
    const float frozen_right = ImGui::TableGetCellBgRect(table, 0).Max.x;

    int c = 0;
    while (c < ncols) {
        const uint32_t comp = t.columns[c].component;
        int first_vis = -1, last_vis = -1;
        int e = c;
        for (; e < ncols && t.columns[e].component == comp; ++e)
            if (ImGui::TableGetColumnFlags(e + 1) & ImGuiTableColumnFlags_IsVisible) {
                if (first_vis < 0) first_vis = e + 1;
                last_vis = e + 1;
            }
        if (first_vis >= 0) {
            ImGui::TableSetColumnIndex(first_vis);
            const ImRect a = ImGui::TableGetCellBgRect(table, first_vis);
            const ImRect b = ImGui::TableGetCellBgRect(table, last_vis);
            ImRect clip(ImMax(a.Min.x, frozen_right), row_y, b.Max.x, row_y + row_h);
            clip.ClipWith(table->InnerClipRect);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(clip.Min, clip.Max, false);
            dl->AddText({ ImMax(a.Min.x, frozen_right) + table->CellPaddingX, row_y + table->RowCellPaddingY },
                        ImGui::GetColorU32(ImGuiCol_Text), t.table->components[comp].name.c_str());
            dl->PopClipRect();
        }
        c = e;
    }
}

void DrawFieldHeaderRow(const ViewTable& t, int ncols)
{
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c <= ncols; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
        if (c > 0 && ImGui::IsItemHovered()) {
            const sheaf::Column& col = *t.columns[c - 1].col;
            ImGui::SetTooltip("%s · %s%s%s", ColumnTitle(t, t.columns[c - 1]).c_str(), TypeName(col.type),
                              (col.flags & sheaf::List) ? " · список" : "", (col.flags & sheaf::Nullable) ? " · nullable" : "");
        }
    }
}

void DrawGrid(ViewerState& s, ViewTable& t)
{
    const int ncols = std::min(sheaf::Narrow<int>(t.columns.size()), kMaxGridColumns);
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
                                | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
    // id на таблицу файла: ImGui хранит ширины, порядок колонок и сортировку по id таблицы.
    if (!ImGui::BeginTable(("grid" + std::to_string(s.current)).c_str(), ncols + 1, flags)) return;

    ImGui::TableSetupScrollFreeze(1, 2);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, 0.0f, 0);
    for (int c = 0; c < ncols; ++c) {
        const ViewColumn& vc = t.columns[c];
        ImGuiTableColumnFlags cf = 0;
        if (vc.col->flags & sheaf::List) cf |= ImGuiTableColumnFlags_NoSort;
        if (t.hidden[vc.component])      cf |= ImGuiTableColumnFlags_Disabled;
        ImGui::TableSetupColumn(vc.col->name.c_str(), cf, 0.0f, sheaf::Narrow<ImGuiID>(c + 1));
    }

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        t.sort_column = specs->SpecsCount ? sheaf::Narrow<int>(specs->Specs[0].ColumnUserID) - 1 : -1;
        t.sort_desc   = specs->SpecsCount && specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        s.model.Rebuild(t);
        specs->SpecsDirty = false;
    }

    DrawComponentHeaderRow(t, ncols);
    DrawFieldHeaderRow(t, ncols);

    const int current = s.current;
    ImGuiListClipper clip;
    clip.Begin(sheaf::Narrow<int>(t.view.size()));
    if (s.scroll_to >= 0 && s.scroll_to < sheaf::Narrow<int>(t.view.size())) clip.IncludeItemByIndex(s.scroll_to);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const uint32_t r = t.view[i];
            const bool row_selected = s.sel.table == current && s.sel.row == r;
            ImGui::TableNextRow();
            if (row_selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
            if (i == s.scroll_to) { ImGui::SetScrollHereY(0.3f); s.scroll_to = -1; }

            ImGui::PushID(sheaf::Narrow<int>(r));
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(t.base + r).c_str(), row_selected && s.sel.column < 0))
                s.sel = { current, r, -1 };

            for (int c = 0; c < ncols; ++c) {
                if (!ImGui::TableSetColumnIndex(c + 1)) continue;
                const ViewColumn& vc = t.columns[c];
                const sheaf::Column& col = *vc.col;
                const bool ref = col.type == sheaf::Type::Ref && !(col.flags & sheaf::List) && !IsAbsent(col, r);
                std::string text = s.model.FormatCell(vc, r);
                if (ref) text = "→ " + text;
                ImGui::PushID(c);
                ImGui::PushStyleColor(ImGuiCol_Text, ref               ? ImGui::GetStyleColorVec4(ImGuiCol_TextLink)
                                                   : IsDefault(vc, r) ? kGrey
                                                                      : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                if (CellSelectable(text, row_selected && s.sel.column == c)) {
                    s.sel = { current, r, c };
                    if (ref) s.navigate_to = col.values[r];
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void DrawSelection(ViewerState& s, const ViewTable& t)
{
    if (s.sel.table != s.current || s.sel.row >= t.table->rows) {
        ImGui::TextColored(kGrey, "Выберите объект или ячейку.");
        return;
    }
    const uint32_t r = s.sel.row;
    ImGui::Text("Объект %u (строка %u таблицы)", t.base + r, r);

    if (s.sel.column < 0) {
        if (!ImGui::BeginTable("object", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            return;
        ImGui::TableSetupColumn("поле", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("значение");
        for (const ViewColumn& vc : t.columns) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ColumnTitle(t, vc).c_str());
            ImGui::TableSetColumnIndex(1);
            if (vc.col->flags & sheaf::List) {
                const size_t b = vc.first[r], n = vc.col->lengths[r];
                if (n == 0) ImGui::TextColored(kGrey, "[]");
                for (size_t i = b; i < b + n; ++i) {
                    if (i > b) ImGui::SameLine();
                    DrawValue(s, *vc.col, i);
                }
            } else if (IsDefault(vc, r)) {
                ImGui::TextColored(kGrey, "%s", s.model.FormatCell(vc, r).c_str());
            } else {
                DrawValue(s, *vc.col, r);
            }
        }
        ImGui::EndTable();
        return;
    }

    const ViewColumn& vc = t.columns[s.sel.column];
    const sheaf::Column& c = *vc.col;
    std::string info = ColumnTitle(t, vc) + " · " + TypeName(c.type);
    if (c.flags & sheaf::List)     info += " · список";
    if (c.flags & sheaf::Nullable) info += " · nullable";
    if (!(c.flags & sheaf::List))  info += " · дефолт " + s.model.FormatValue(c, c.def);
    info += std::string(" · записано: ") + EncodingName(c.encoding) + ", " + std::to_string(c.encoded_bytes) + " Б";
    ImGui::TextUnformatted(info.c_str());
    if (c.type == sheaf::Type::Str && !(c.flags & sheaf::List) && !IsAbsent(c, r))
        ImGui::TextColored(kGrey, "номер строки в файле: %u", c.values[r]);
    ImGui::Separator();

    if (!(c.flags & sheaf::List)) { DrawValue(s, c, r); return; }
    const size_t b = vc.first[r], n = c.lengths[r];
    ImGui::Text("%zu элементов", n);
    for (size_t i = b; i < b + n; ++i) {
        ImGui::TextColored(kGrey, "%zu:", i - b);
        ImGui::SameLine();
        DrawValue(s, c, i);
    }
}

} // namespace

void OpenFile(ViewerState& s, const std::string& path)
{
    std::string error;
    if (!s.model.Open(path, error)) { s.error = error; return; }
    s.error.clear();
    s.current = -1;
    s.scroll_to = -1;
    if (!s.model.Tables().empty()) SelectTable(s, 0);
}

void DrawViewer(ViewerState& s)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##viewer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (s.navigate_to >= 0) {
        Navigate(s, sheaf::Narrow<uint32_t>(s.navigate_to));
        s.navigate_to = -1;
    }

    if (ImGui::Button("Открыть…") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
        s.open_requested = true;
    ImGui::SameLine();
    if (s.model.IsOpen())
        ImGui::Text("%s · %zu Б · таблиц %zu · объектов %u", s.model.Path().c_str(), s.model.Bytes(),
                    s.model.Tables().size(), s.model.TotalRows());
    else
        ImGui::TextColored(kGrey, "Перетащите .sheaf в окно или нажмите Ctrl+O.");
    if (!s.error.empty()) ImGui::TextColored(kRed, "%s", s.error.c_str());

    if (s.model.IsOpen() && s.current >= 0) {
        ImGui::BeginChild("tables", { 240, 0 }, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        DrawTableList(s);
        ImGui::EndChild();
        ImGui::SameLine();

        ViewTable& t = s.model.Tables()[s.current];
        ImGui::BeginGroup();
        std::string title;
        for (const sheaf::Component& c : t.table->components) title += (title.empty() ? "" : " + ") + c.name;
        ImGui::Text("%s · объектов %u", title.c_str(), t.table->rows);
        if (t.filter.column >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "· подходит %u", t.matched);
        }
        if (sheaf::Narrow<int>(t.columns.size()) > kMaxGridColumns) {
            ImGui::SameLine();
            ImGui::TextColored(kRed, "· показаны первые %d полей из %zu", kMaxGridColumns, t.columns.size());
        }
        DrawToolbar(s, t);
        ImGui::BeginChild("grid", { 0, ImGui::GetContentRegionAvail().y * 0.7f }, ImGuiChildFlags_ResizeY);
        DrawGrid(s, t);
        ImGui::EndChild();
        ImGui::BeginChild("selection", { 0, 0 }, ImGuiChildFlags_Borders);
        DrawSelection(s, s.model.Tables()[s.current]);
        ImGui::EndChild();
        ImGui::EndGroup();
    }
    ImGui::End();
}
