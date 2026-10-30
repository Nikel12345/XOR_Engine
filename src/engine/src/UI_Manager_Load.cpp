#include "PCH.h"
#include "UI_Manager.h"
#include "UI_Markup.h"
#include "FontManager.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>

struct UIDeclaredValues {
    UIStyle     style;
    std::string material, quad, font_family;
};

// Весь документ переводится в такие записи до первого вызова Root, чтобы ошибка в файле не оставила
// полдерева.
struct UILoadedNode : UIDeclaredValues {
    enum class Kind : uint8_t { Root, Box, Text };
    Kind        kind   = Kind::Box;
    int32_t     parent = -1;
    std::string text, id;
    std::vector<std::string>        classes;
    std::vector<UITemplatePart>     text_template;
    std::vector<UIBoundDeclaration> bound_declarations;
};

struct UILoadedVar {
    std::string name;
    std::string value;
    UIVarId     id;
};

struct UILoadState {
    const std::string&        path;
    const UIMarkupDocument&   document;
    const FontManager*        font_manager;
    std::vector<UILoadedVar>  vars;
    std::vector<UILoadedNode> nodes;
    std::vector<std::string>  errors;

    void Error(uint32_t line, const std::string& message)
    {
        errors.push_back(path + ":" + std::to_string(line) + ": " + message);
    }

    const UILoadedVar* FindVar(std::string_view name) const
    {
        for (const UILoadedVar& var : vars) if (var.name == name) return &var;
        return nullptr;
    }

    std::string_view ValueOf(UIVarId id) const
    {
        for (const UILoadedVar& var : vars) if (var.id == id) return var.value;
        return {};
    }
};

static std::string_view TrimSpaces(std::string_view value)
{
    const size_t begin = value.find_first_not_of(" \t\r\n\f");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n\f") + 1 - begin);
}

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

static bool IsVarNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool IsVarName(std::string_view name)
{
    return !name.empty() && !(name[0] >= '0' && name[0] <= '9') && std::ranges::all_of(name, IsVarNameChar);
}

enum class UIDeclarationResult : uint8_t { Applied, UnknownProperty, InvalidValue };

// Свойство, которое элементу не к чему применить, пропускается при сборке: его приносят общие правила
// вроде *.
static UIDeclarationResult ApplyDeclaration(std::string_view property, std::string_view value, UIDeclaredValues& values)
{
    UIStyle& style = values.style;
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
        valid = ParseName(value, values.material);
        if (values.material == "none") values.material.clear();
    }
    else if (property == "quad")            valid = ParseName(value, values.quad);
    else if (property == "font-family")     valid = ParseName(value, values.font_family);
    else return UIDeclarationResult::UnknownProperty;
    return valid ? UIDeclarationResult::Applied : UIDeclarationResult::InvalidValue;
}

static bool HasVars(const std::vector<UITemplatePart>& parts)
{
    return std::ranges::any_of(parts, [](const UITemplatePart& part) { return bool(part.var); });
}

template <class ValueOf>
static std::string RenderTemplate(const std::vector<UITemplatePart>& parts, const ValueOf& value_of)
{
    std::string out;
    for (const UITemplatePart& part : parts) {
        out += part.literal;
        if (part.var) out += value_of(part.var);
    }
    return out;
}

// Часть шаблона — текст и следом за ним переменная; у последней части переменной нет.
static bool ParseTemplate(UILoadState& state, std::string_view source, uint32_t line, std::vector<UITemplatePart>& parts)
{
    parts.assign(1, UITemplatePart{});
    while (true) {
        const size_t open = source.find("{{");
        if (open == std::string_view::npos) { parts.back().literal += source; return true; }
        parts.back().literal += source.substr(0, open);
        const size_t close = source.find("}}", open + 2);
        if (close == std::string_view::npos) { state.Error(line, "'{{' is not closed with '}}'"); return false; }
        const std::string_view name = TrimSpaces(source.substr(open + 2, close - open - 2));
        const UILoadedVar* var = state.FindVar(name);
        if (!var) { state.Error(line, "variable '" + std::string(name) + "' is not declared with <var>"); return false; }
        parts.back().var = var->id;
        parts.emplace_back();
        source.remove_prefix(close + 2);
    }
}

static bool ReadVar(UILoadState& state, const UIMarkupNode& element, UILoadedVar& var)
{
    for (const UIMarkupAttribute& attribute : element.attributes)
        if (attribute.name != "name" && attribute.name != "default")
            state.Error(attribute.line, "attribute '" + attribute.name + "' is not supported on <var>");
    if (!element.children.empty()) state.Error(element.line, "<var> holds nothing: the value goes in default");

    const UIMarkupAttribute* name = element.Attribute("name");
    if (!name) { state.Error(element.line, "<var> needs name"); return false; }
    if (!IsVarName(name->value)) {
        state.Error(name->line, "variable name '" + name->value + "' must be letters, digits and _, not starting with a digit");
        return false;
    }
    if (state.FindVar(name->value)) { state.Error(name->line, "variable '" + name->value + "' is declared twice"); return false; }
    var.name = name->value;
    if (const UIMarkupAttribute* default_value = element.Attribute("default")) var.value = default_value->value;
    return true;
}

static void LoadDeclarations(UILoadState& state, const UIMarkupNode& element, UILoadedNode& node, bool& material_bound)
{
    const auto value_of = [&state](UIVarId id) { return state.ValueOf(id); };
    std::vector<UIBoundDeclaration> declarations;
    bool bound = false;
    for (const UIMarkupDeclaration* declaration : ResolveUIStyle(state.document, element)) {
        UIBoundDeclaration& entry = declarations.emplace_back(UIBoundDeclaration{ declaration->property, {}, declaration->line });
        if (!ParseTemplate(state, declaration->value, declaration->line, entry.value)) continue;
        const bool has_vars = HasVars(entry.value);
        if (has_vars && (entry.property == "quad" || entry.property == "font-family")) {
            state.Error(declaration->line, "'" + entry.property + "' takes no {{ }}: it is read once at load");
            continue;
        }
        bound          = bound || has_vars;
        material_bound = material_bound || (has_vars && entry.property == "material");

        const std::string value = RenderTemplate(entry.value, value_of);
        switch (ApplyDeclaration(entry.property, value, node)) {
            case UIDeclarationResult::Applied:
                break;
            case UIDeclarationResult::UnknownProperty:
                state.Error(declaration->line, "unknown property '" + entry.property + "'");
                break;
            case UIDeclarationResult::InvalidValue:
                state.Error(declaration->line, "invalid value '" + value + "' for '" + entry.property + "'" +
                                               (has_vars ? " from '" + declaration->value + "'" : ""));
                break;
        }
    }
    if (bound) node.bound_declarations = std::move(declarations);
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

    for (const UIMarkupAttribute& attribute : element.attributes) {
        if (attribute.name != "id" && attribute.name != "class")
            state.Error(attribute.line, "attribute '" + attribute.name + "' is not supported on <" + element.tag + ">");
        else if (attribute.value.find("{{") != std::string::npos)
            state.Error(attribute.line, "attribute '" + attribute.name + "' takes no {{ }}");
    }
    if (const UIMarkupAttribute* id = element.Attribute("id")) node.id = id->value;
    if (const UIMarkupAttribute* classes = element.Attribute("class")) node.classes = SplitClasses(classes->value);

    bool material_bound = false;
    LoadDeclarations(state, element, node, material_bound);

    if (node.kind == UILoadedNode::Kind::Text) {
        for (const UIMarkupNode& child : element.children)
            if (!child.IsText() && child.tag != "br")
                state.Error(child.line, "<text> holds only text and <br/>, not <" + child.tag + ">");
        std::vector<UITemplatePart> text_template;
        if (ParseTemplate(state, UIMarkupText(element), element.line, text_template)) {
            node.text = RenderTemplate(text_template, [&state](UIVarId id) { return state.ValueOf(id); });
            if (HasVars(text_template)) node.text_template = std::move(text_template);
        }
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
    if (node.kind == UILoadedNode::Kind::Box && (!node.material.empty() || material_bound) && node.quad.empty())
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
    for (const UIMarkupNode& element : document->root.children) {
        if (element.tag != "var") continue;
        UILoadedVar var;
        if (!ReadVar(state, element, var)) continue;
        var.id = InternVar(var.name);
        if (const UIVar* existing = variables.Get(var.id); existing->assigned) var.value = existing->value;
        state.vars.push_back(std::move(var));
    }
    LoadElement(state, document->root, -1);
    if (!state.errors.empty()) {
        for (const std::string& error : state.errors) SDL_Log("UI_Manager::Load: %s", error.c_str());
        return false;
    }

    for (const UILoadedVar& var : state.vars) variables.Get(var.id)->value = var.value;

    std::vector<Node> created_nodes(state.nodes.size());
    for (size_t i = 0; i < state.nodes.size(); ++i) {
        UILoadedNode& loaded = state.nodes[i];
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

        if (!loaded.text_template.empty())
            AddBinding({ UIBindingKind::Text, node, std::move(loaded.text_template) });
        if (!loaded.bound_declarations.empty())
            AddBinding({ UIBindingKind::Style, node, {}, std::move(loaded.bound_declarations) });
    }
    document_path = path;
    return true;
}

void UI_Manager::AddBinding(UIBinding binding)
{
    const uint32_t index = safe_u32(bindings.size());
    const auto link = [this, index](const std::vector<UITemplatePart>& parts) {
        for (const UITemplatePart& part : parts) {
            UIVar* var = variables.Get(part.var);
            if (var && (var->bindings.empty() || var->bindings.back() != index)) var->bindings.push_back(index);
        }
    };
    link(binding.text);
    for (const UIBoundDeclaration& declaration : binding.declarations) link(declaration.value);
    bindings.push_back(std::move(binding));
}

UIVarId UI_Manager::InternVar(const std::string& name)
{
    const UIVarId id = variables.Intern(name);
    if (id && !variables.Get(id)) variables.Put(id, std::make_unique<UIVar>());
    return id;
}

void UI_Manager::SetVar(UIVarId id, std::string_view value)
{
    UIVar* var = variables.Get(id);
    if (!var) return;
    var->assigned = true;
    if (var->value == value) return;
    var->value = value;
    for (uint32_t index : var->bindings) {
        UIBinding& binding = bindings[index];
        if (binding.queued) continue;
        binding.queued = true;
        dirty_bindings.push_back(index);
    }
}

void UI_Manager::ApplyDirtyBindings(EngineContext* ctx, const FontManager* font_manager)
{
    const auto value_of = [this](UIVarId id) {
        const UIVar* var = variables.Get(id);
        return var ? std::string_view(var->value) : std::string_view();
    };
    for (uint32_t index : dirty_bindings) {
        UIBinding& binding = bindings[index];
        binding.queued = false;
        if (binding.kind == UIBindingKind::Text) {
            SetText(binding.node, RenderTemplate(binding.text, value_of), font_manager);
            continue;
        }

        // Стиль собирается заново из всех объявлений узла: связанное объявление может перекрываться
        // следующими, в том числе частично (padding и padding-left).
        UIDeclaredValues values;
        for (const UIBoundDeclaration& declaration : binding.declarations) {
            const std::string value = RenderTemplate(declaration.value, value_of);
            // Разбор пишет в поле до проверки диапазона: неверное значение применяется к копии.
            UIDeclaredValues candidate = values;
            if (ApplyDeclaration(declaration.property, value, candidate) == UIDeclarationResult::Applied)
                values = std::move(candidate);
            else
                SDL_Log("UI_Manager: %s:%u: invalid value '%s' for '%s'", document_path.c_str(), declaration.line,
                        value.c_str(), declaration.property.c_str());
        }
        SetNodeStyle(binding.node, values.style);
        SetNodeMaterial(ctx, binding.node, values.material);
    }
    dirty_bindings.clear();
}
