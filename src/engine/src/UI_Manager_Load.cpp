#include "PCH.h"
#include "UI_Manager.h"
#include "UI_Markup.h"
#include "FontManager.h"
#include <charconv>
#include <fstream>
#include <iterator>

// Весь документ переводится в такие записи до первого вызова Root, чтобы ошибка в файле не оставила
// полдерева.
struct UILoadedNode {
    enum class Kind : uint8_t { Root, Box, Text };
    Kind        kind   = Kind::Box;
    int32_t     parent = -1;
    UIStyle     style;
    std::string material, quad, font_family, text, id;
    std::vector<std::string> classes;
};

struct UILoadState {
    const std::string&        path;
    const UIMarkupDocument&   document;
    const FontManager*        font_manager;
    std::vector<UILoadedNode> nodes;
    std::vector<std::string>  errors;

    void Error(uint32_t line, const std::string& message)
    {
        errors.push_back(path + ":" + std::to_string(line) + ": " + message);
    }
};

static bool ParseFloat(std::string_view value, float& number)
{
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    return !value.empty() && error == std::errc{} && end == value.data() + value.size() && std::isfinite(number);
}

static bool ParseLength(std::string_view value, float& px)
{
    if (value == "0") { px = 0.0f; return true; }
    return value.ends_with("px") && ParseFloat(value.substr(0, value.size() - 2), px);
}

static bool ParseSize(std::string_view value, UISize& mode, float& size)
{
    if (value == "auto") { mode = UISize::Auto; size = 0.0f; return true; }
    if (value.ends_with('%')) {
        if (!ParseFloat(value.substr(0, value.size() - 1), size) || size < 0.0f) return false;
        mode = UISize::Percent;
        return true;
    }
    if (!ParseLength(value, size) || size < 0.0f) return false;
    mode = UISize::Points;
    return true;
}

static bool ParseEdge(std::string_view value, float& px, bool allow_negative)
{
    return ParseLength(value, px) && (allow_negative || px >= 0.0f);
}

static bool ParseEdges(std::string_view value, UIEdges& edges, bool allow_negative)
{
    float lengths[4] = {};
    size_t count = 0;
    while (true) {
        const size_t begin = value.find_first_not_of(" \t\r\n\f");
        if (begin == std::string_view::npos) break;
        value.remove_prefix(begin);
        const size_t end = std::min(value.find_first_of(" \t\r\n\f"), value.size());
        if (count == 4 || !ParseEdge(value.substr(0, end), lengths[count], allow_negative)) return false;
        ++count;
        value.remove_prefix(end);
    }
    switch (count) {
        case 1:  edges = { lengths[0], lengths[0], lengths[0], lengths[0] }; return true;
        case 2:  edges = { lengths[0], lengths[1], lengths[0], lengths[1] }; return true;
        case 3:  edges = { lengths[0], lengths[1], lengths[2], lengths[1] }; return true;
        case 4:  edges = { lengths[0], lengths[1], lengths[2], lengths[3] }; return true;
        default: return false;
    }
}

static bool ParseDirection(std::string_view value, UIDir& direction)
{
    if (value == "row")    { direction = UIDir::Row;    return true; }
    if (value == "column") { direction = UIDir::Column; return true; }
    return false;
}

static bool ParseJustify(std::string_view value, UIJustify& justify)
{
    if (value == "flex-start" || value == "start") { justify = UIJustify::Start;        return true; }
    if (value == "center")                         { justify = UIJustify::Center;       return true; }
    if (value == "flex-end" || value == "end")     { justify = UIJustify::End;          return true; }
    if (value == "space-between")                  { justify = UIJustify::SpaceBetween; return true; }
    return false;
}

static bool ParseAlign(std::string_view value, UIAlign& align)
{
    if (value == "flex-start" || value == "start") { align = UIAlign::Start;   return true; }
    if (value == "center")                         { align = UIAlign::Center;  return true; }
    if (value == "flex-end" || value == "end")     { align = UIAlign::End;     return true; }
    if (value == "stretch")                        { align = UIAlign::Stretch; return true; }
    return false;
}

static std::vector<std::string> SplitClasses(std::string_view value)
{
    std::vector<std::string> classes;
    while (true) {
        const size_t begin = value.find_first_not_of(" \t\r\n\f");
        if (begin == std::string_view::npos) return classes;
        value.remove_prefix(begin);
        const size_t end = std::min(value.find_first_of(" \t\r\n\f"), value.size());
        classes.emplace_back(value.substr(0, end));
        value.remove_prefix(end);
    }
}

static bool ParseName(std::string_view value, std::string& name)
{
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    name = value;
    return !name.empty();
}

// Свойство, которое элементу не к чему применить, пропускается при сборке: его приносят общие правила
// вроде *.
static void ApplyDeclaration(UILoadState& state, UILoadedNode& node, const UIMarkupDeclaration& declaration)
{
    const std::string_view property = declaration.property;
    const std::string_view value    = declaration.value;
    UIStyle& style = node.style;
    bool valid = true;
    if      (property == "flex-direction")  valid = ParseDirection(value, style.direction);
    else if (property == "justify-content") valid = ParseJustify(value, style.justify);
    else if (property == "align-items")     valid = ParseAlign(value, style.align);
    else if (property == "width")           valid = ParseSize(value, style.width_mode, style.width);
    else if (property == "height")          valid = ParseSize(value, style.height_mode, style.height);
    else if (property == "flex-grow")       valid = ParseFloat(value, style.grow) && style.grow >= 0.0f;
    else if (property == "gap")             valid = ParseEdge(value, style.gap, false);
    else if (property == "padding")         valid = ParseEdges(value, style.padding, false);
    else if (property == "padding-top")     valid = ParseEdge(value, style.padding.top,    false);
    else if (property == "padding-right")   valid = ParseEdge(value, style.padding.right,  false);
    else if (property == "padding-bottom")  valid = ParseEdge(value, style.padding.bottom, false);
    else if (property == "padding-left")    valid = ParseEdge(value, style.padding.left,   false);
    else if (property == "margin")          valid = ParseEdges(value, style.margin, true);
    else if (property == "margin-top")      valid = ParseEdge(value, style.margin.top,    true);
    else if (property == "margin-right")    valid = ParseEdge(value, style.margin.right,  true);
    else if (property == "margin-bottom")   valid = ParseEdge(value, style.margin.bottom, true);
    else if (property == "margin-left")     valid = ParseEdge(value, style.margin.left,   true);
    else if (property == "material") {
        valid = ParseName(value, node.material);
        if (node.material == "none") node.material.clear();
    }
    else if (property == "quad")            valid = ParseName(value, node.quad);
    else if (property == "font-family")     valid = ParseName(value, node.font_family);
    else {
        state.Error(declaration.line, "unknown property '" + declaration.property + "'");
        return;
    }
    if (!valid) state.Error(declaration.line, "invalid value '" + declaration.value + "' for '" + declaration.property + "'");
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

    for (const UIMarkupAttribute& attribute : element.attributes)
        if (attribute.name != "id" && attribute.name != "class")
            state.Error(attribute.line, "attribute '" + attribute.name + "' is not supported on <" + element.tag + ">");
    if (const UIMarkupAttribute* id = element.Attribute("id")) node.id = id->value;
    if (const UIMarkupAttribute* classes = element.Attribute("class")) node.classes = SplitClasses(classes->value);

    for (const UIMarkupDeclaration* declaration : ResolveUIStyle(state.document, element))
        ApplyDeclaration(state, node, *declaration);

    if (node.kind == UILoadedNode::Kind::Text) {
        for (const UIMarkupNode& child : element.children)
            if (!child.IsText() && child.tag != "br")
                state.Error(child.line, "<text> holds only text and <br/>, not <" + child.tag + ">");
        node.text = UIMarkupText(element);
        node.style.padding = {};
        if (node.font_family.empty())
            state.Error(element.line, "<text> needs font-family");
        else if (!state.font_manager->FontIdOf(node.font_family))
            state.Error(element.line, "font '" + node.font_family + "' is not created");
        if (node.material.empty()) state.Error(element.line, "<text> needs material");
        if (node.quad.empty())     state.Error(element.line, "<text> needs quad");
        state.nodes.push_back(std::move(node));
        return;
    }
    if (node.kind == UILoadedNode::Kind::Box && !node.material.empty() && node.quad.empty())
        state.Error(element.line, "<box> with material needs quad");

    const int32_t index = safe_size_i(state.nodes.size());
    state.nodes.push_back(std::move(node));
    for (const UIMarkupNode& child : element.children) {
        if (child.IsText()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, index);
    }
}

bool UI_Manager::Load(const FontManager* font_manager, const std::string& path)
{
    if (!font_manager) { SDL_Log("UI_Manager::Load: no FontManager"); return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { SDL_Log("UI_Manager::Load: cannot open '%s'", path.c_str()); return false; }
    const std::string source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };

    const auto document = ParseUIMarkup(source);
    if (!document) {
        SDL_Log("UI_Manager::Load: %s:%u: %s", path.c_str(), document.error().line, document.error().message.c_str());
        return false;
    }

    UILoadState state{ path, *document, font_manager };
    LoadElement(state, document->root, -1);
    if (!state.errors.empty()) {
        for (const std::string& error : state.errors) SDL_Log("UI_Manager::Load: %s", error.c_str());
        return false;
    }

    std::vector<Node> created_nodes(state.nodes.size());
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        const UILoadedNode& loaded = state.nodes[i];
        Node node = kInvalid;
        switch (loaded.kind) {
            case UILoadedNode::Kind::Root:
                node = Root(loaded.style);
                break;
            case UILoadedNode::Kind::Box:
                node = Box(created_nodes[safe_i_u32(loaded.parent)], loaded.style, loaded.material, loaded.quad);
                break;
            case UILoadedNode::Kind::Text:
                node = Text(created_nodes[safe_i_u32(loaded.parent)], loaded.style, loaded.text, loaded.material,
                            loaded.quad, font_manager->FontIdOf(loaded.font_family), font_manager);
                break;
        }
        nodes[node].id      = loaded.id;
        nodes[node].classes = loaded.classes;
        created_nodes[i] = node;
    }
    return true;
}
