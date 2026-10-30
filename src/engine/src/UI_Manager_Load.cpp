#include "PCH.h"
#include "UI_Manager.h"
#include "UI_Markup.h"
#include "FontManager.h"
#include <charconv>
#include <fstream>
#include <iterator>

// Узел, собранный из элемента файла. Весь документ переводится в такие записи до первого вызова Root,
// чтобы ошибка в файле не оставила полдерева.
struct UILoadedNode {
    enum class Kind : uint8_t { Root, Box, Text };
    Kind        kind   = Kind::Box;
    int32_t     parent = -1;   // индекс в списке записей
    UIStyle     style;
    std::string material, quad, font_family, text, id;
};

struct UILoadState {
    const std::string&        path;
    const UIMarkupDocument&   doc;
    const FontManager*        fm;
    std::vector<UILoadedNode> nodes;
    std::vector<std::string>  errors;

    void Error(uint32_t line, const std::string& message)
    {
        errors.push_back(path + ":" + std::to_string(line) + ": " + message);
    }
};

static bool ParseFloat(std::string_view s, float& out)
{
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return !s.empty() && ec == std::errc{} && end == s.data() + s.size() && std::isfinite(out);
}

// Как в CSS: длина в px, ноль можно без единиц.
static bool ParseLength(std::string_view s, float& px)
{
    if (s == "0") { px = 0.0f; return true; }
    return s.ends_with("px") && ParseFloat(s.substr(0, s.size() - 2), px);
}

static bool ParseSize(std::string_view s, UISize& mode, float& value)
{
    if (s == "auto") { mode = UISize::Auto; value = 0.0f; return true; }
    if (s.ends_with('%')) {
        if (!ParseFloat(s.substr(0, s.size() - 1), value) || value < 0.0f) return false;
        mode = UISize::Percent;
        return true;
    }
    if (!ParseLength(s, value) || value < 0.0f) return false;
    mode = UISize::Points;
    return true;
}

static bool ParseEdge(std::string_view s, float& px, bool allow_negative)
{
    return ParseLength(s, px) && (allow_negative || px >= 0.0f);
}

// CSS-сокращение: 1 значение — все края, 2 — верх/низ и бока, 3 — верх, бока, низ, 4 — по часовой с верха.
static bool ParseEdges(std::string_view s, UIEdges& e, bool allow_negative)
{
    float v[4] = {};
    size_t count = 0;
    while (true) {
        const size_t begin = s.find_first_not_of(" \t\r\n\f");
        if (begin == std::string_view::npos) break;
        s.remove_prefix(begin);
        const size_t end = std::min(s.find_first_of(" \t\r\n\f"), s.size());
        if (count == 4 || !ParseEdge(s.substr(0, end), v[count], allow_negative)) return false;
        ++count;
        s.remove_prefix(end);
    }
    switch (count) {
        case 1:  e = { v[0], v[0], v[0], v[0] }; return true;
        case 2:  e = { v[0], v[1], v[0], v[1] }; return true;
        case 3:  e = { v[0], v[1], v[2], v[1] }; return true;
        case 4:  e = { v[0], v[1], v[2], v[3] }; return true;
        default: return false;
    }
}

static bool ParseDirection(std::string_view s, UIDir& dir)
{
    if (s == "row")    { dir = UIDir::Row;    return true; }
    if (s == "column") { dir = UIDir::Column; return true; }
    return false;
}

static bool ParseJustify(std::string_view s, UIJustify& justify)
{
    if (s == "flex-start" || s == "start") { justify = UIJustify::Start;        return true; }
    if (s == "center")                     { justify = UIJustify::Center;       return true; }
    if (s == "flex-end" || s == "end")     { justify = UIJustify::End;          return true; }
    if (s == "space-between")              { justify = UIJustify::SpaceBetween; return true; }
    return false;
}

static bool ParseAlign(std::string_view s, UIAlign& align)
{
    if (s == "flex-start" || s == "start") { align = UIAlign::Start;   return true; }
    if (s == "center")                     { align = UIAlign::Center;  return true; }
    if (s == "flex-end" || s == "end")     { align = UIAlign::End;     return true; }
    if (s == "stretch")                    { align = UIAlign::Stretch; return true; }
    return false;
}

// Имя ресурса. Кавычки, которые CSS допускает в font-family, снимаются.
static bool ParseName(std::string_view s, std::string& name)
{
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    name = s;
    return !name.empty();
}

// Свойство, которое элементу не к чему применить (font-family у бокса, padding у текста), пропускается
// при сборке: его приносят общие правила вроде *. Значение проверяется всегда.
static void ApplyDeclaration(UILoadState& state, UILoadedNode& node, const UIMarkupDeclaration& d)
{
    const std::string_view p = d.property;
    const std::string_view v = d.value;
    UIStyle& s = node.style;
    bool ok = true;
    if      (p == "flex-direction")  ok = ParseDirection(v, s.dir);
    else if (p == "justify-content") ok = ParseJustify(v, s.justify);
    else if (p == "align-items")     ok = ParseAlign(v, s.align);
    else if (p == "width")           ok = ParseSize(v, s.wmode, s.w);
    else if (p == "height")          ok = ParseSize(v, s.hmode, s.h);
    else if (p == "flex-grow")       ok = ParseFloat(v, s.grow) && s.grow >= 0.0f;
    else if (p == "gap")             ok = ParseEdge(v, s.gap, false);
    else if (p == "padding")         ok = ParseEdges(v, s.padding, false);
    else if (p == "padding-top")     ok = ParseEdge(v, s.padding.top,    false);
    else if (p == "padding-right")   ok = ParseEdge(v, s.padding.right,  false);
    else if (p == "padding-bottom")  ok = ParseEdge(v, s.padding.bottom, false);
    else if (p == "padding-left")    ok = ParseEdge(v, s.padding.left,   false);
    else if (p == "margin")          ok = ParseEdges(v, s.margin, true);
    else if (p == "margin-top")      ok = ParseEdge(v, s.margin.top,    true);
    else if (p == "margin-right")    ok = ParseEdge(v, s.margin.right,  true);
    else if (p == "margin-bottom")   ok = ParseEdge(v, s.margin.bottom, true);
    else if (p == "margin-left")     ok = ParseEdge(v, s.margin.left,   true);
    else if (p == "material") {
        ok = ParseName(v, node.material);
        if (node.material == "none") node.material.clear();
    }
    else if (p == "quad")            ok = ParseName(v, node.quad);
    else if (p == "font-family")     ok = ParseName(v, node.font_family);
    else {
        state.Error(d.line, "unknown property '" + d.property + "'");
        return;
    }
    if (!ok) state.Error(d.line, "invalid value '" + d.value + "' for '" + d.property + "'");
}

static void LoadElement(UILoadState& state, const UIMarkupNode& element, int32_t parent)
{
    UILoadedNode node;
    node.parent = parent;
    if (parent < 0) {
        if (element.tag != "ui") { state.Error(element.line, "root element must be <ui>, not <" + element.tag + ">"); return; }
        node.kind = UILoadedNode::Kind::Root;
    }
    else if (element.tag == "box")  node.kind = UILoadedNode::Kind::Box;
    else if (element.tag == "text") node.kind = UILoadedNode::Kind::Text;
    else if (element.tag == "var") {
        if (state.nodes[safe_i_u32(parent)].kind != UILoadedNode::Kind::Root)
            state.Error(element.line, "<var> belongs directly in <ui>");
        return;
    }
    else if (element.tag == "br") { state.Error(element.line, "<br/> belongs in <text>"); return; }
    else { state.Error(element.line, "unknown tag <" + element.tag + ">"); return; }

    for (const UIMarkupAttribute& a : element.attributes)
        if (a.name != "id" && a.name != "class")
            state.Error(a.line, "attribute '" + a.name + "' is not supported on <" + element.tag + ">");
    if (const UIMarkupAttribute* id = element.Attribute("id")) node.id = id->value;

    for (const UIMarkupDeclaration* d : ResolveUIStyle(state.doc, element))
        ApplyDeclaration(state, node, *d);

    if (node.kind == UILoadedNode::Kind::Text) {
        for (const UIMarkupNode& child : element.children)
            if (!child.tag.empty() && child.tag != "br")
                state.Error(child.line, "<text> holds only text and <br/>, not <" + child.tag + ">");
        node.text = UIMarkupText(element);
        node.style.padding = {};
        if (node.font_family.empty())                   state.Error(element.line, "<text> needs font-family");
        else if (!state.fm->FontIdOf(node.font_family)) state.Error(element.line, "font '" + node.font_family + "' is not created");
        if (node.material.empty()) state.Error(element.line, "<text> needs material");
        if (node.quad.empty())     state.Error(element.line, "<text> needs quad");
        state.nodes.push_back(std::move(node));
        return;
    }
    if (node.kind == UILoadedNode::Kind::Box && !node.material.empty() && node.quad.empty())
        state.Error(element.line, "<box> with material needs quad");

    const int32_t self = safe_size_i(state.nodes.size());
    state.nodes.push_back(std::move(node));
    for (const UIMarkupNode& child : element.children) {
        if (child.tag.empty()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, self);
    }
}

bool UI_Manager::Load(const FontManager* fm, const std::string& path)
{
    if (!fm) { SDL_Log("UI_Manager::Load: no FontManager"); return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { SDL_Log("UI_Manager::Load: cannot open '%s'", path.c_str()); return false; }
    const std::string source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };

    const auto doc = ParseUIMarkup(source);
    if (!doc) {
        SDL_Log("UI_Manager::Load: %s:%u: %s", path.c_str(), doc.error().line, doc.error().message.c_str());
        return false;
    }

    UILoadState state{ path, *doc, fm };
    LoadElement(state, doc->root, -1);
    if (!state.errors.empty()) {
        for (const std::string& e : state.errors) SDL_Log("UI_Manager::Load: %s", e.c_str());
        return false;
    }

    std::vector<Node> made(state.nodes.size());
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        const UILoadedNode& s = state.nodes[i];
        Node n = kInvalid;
        switch (s.kind) {
            case UILoadedNode::Kind::Root: n = Root(s.style); break;
            case UILoadedNode::Kind::Box:  n = Box(made[safe_i_u32(s.parent)], s.style, s.material, s.quad); break;
            case UILoadedNode::Kind::Text:
                n = Text(made[safe_i_u32(s.parent)], s.style, s.text, s.material, s.quad, fm->FontIdOf(s.font_family), fm);
                break;
        }
        nodes[n].id = s.id;
        made[i] = n;
    }
    return true;
}
