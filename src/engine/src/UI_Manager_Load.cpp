#include "PCH.h"
#include "UI_Manager.h"
#include "UI_Markup.h"
#include "FontManager.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>

struct UIDeclaredValues {
    UIStyle     style;
    std::string material, quad, font_family, caret_material, caret_quad;
};

struct UILoadedVar {
    std::string name;
    std::string default_value;
    std::string value;
    UIVarId     id;
};

// Весь документ переводится в элементы до первого вызова Root, чтобы ошибка в файле не оставила полдерева.
struct UILoadState {
    const std::string&       path;
    const UIMarkupDocument&  document;
    const FontManager*       font_manager;
    std::vector<UILoadedVar> vars;
    std::vector<UIElement>   elements;
    std::vector<std::string> loop_indices;
    std::vector<std::string> errors;

    void Error(uint32_t line, const std::string& message)
    {
        errors.push_back(path + ":" + std::to_string(line) + ": " + message);
    }

    const UILoadedVar* FindVar(std::string_view name) const
    {
        for (const UILoadedVar& var : vars) if (var.name == name) return &var;
        return nullptr;
    }

    int32_t FindLoopIndex(std::string_view name) const
    {
        for (size_t depth = loop_indices.size(); depth-- > 0;)
            if (loop_indices[depth] == name) return safe_size_i(depth);
        return -1;
    }

    std::string ValueOf(const UITemplatePart& part) const
    {
        if (part.loop >= 0) return "0";
        for (const UILoadedVar& var : vars)
            if (var.id == part.var) return part.key.empty() ? var.value : var.default_value;
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

static bool ParseCaretLength(std::string_view value, UISize& mode, float& length)
{
    return ParseSize(value, mode, length) && mode != UISize::Auto;
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

template <class Value>
struct UIKeyword {
    std::string_view name;
    Value            value;
};

// Первое имя значения — то, что пишет Save; следующие с тем же значением — синонимы для разбора.
static constexpr UIKeyword<UIDisplay> kDisplayNames[] = { { "flex", UIDisplay::Flex }, { "none", UIDisplay::None } };
static constexpr UIKeyword<UIDir>     kDirectionNames[] = { { "row", UIDir::Row }, { "column", UIDir::Column } };
static constexpr UIKeyword<UIJustify> kJustifyNames[] = {
    { "flex-start", UIJustify::Start }, { "center", UIJustify::Center }, { "flex-end", UIJustify::End },
    { "space-between", UIJustify::SpaceBetween }, { "start", UIJustify::Start }, { "end", UIJustify::End },
};
static constexpr UIKeyword<UIAlign> kAlignNames[] = {
    { "flex-start", UIAlign::Start }, { "center", UIAlign::Center }, { "flex-end", UIAlign::End },
    { "stretch", UIAlign::Stretch }, { "start", UIAlign::Start }, { "end", UIAlign::End },
};

template <class Value, size_t Count>
static bool ParseKeyword(std::string_view text, const UIKeyword<Value> (&names)[Count], Value& value)
{
    for (const UIKeyword<Value>& keyword : names)
        if (keyword.name == text) { value = keyword.value; return true; }
    return false;
}

template <class Value, size_t Count>
static std::string_view KeywordOf(const UIKeyword<Value> (&names)[Count], Value value)
{
    for (const UIKeyword<Value>& keyword : names)
        if (keyword.value == value) return keyword.name;
    return {};
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


static UIDeclarationResult ApplyDeclaration(std::string_view property, std::string_view value, UIDeclaredValues& values)
{
    UIStyle& style = values.style;
    bool valid = true;
    if      (property == "display")         valid = ParseKeyword(value, kDisplayNames,   style.display);
    else if (property == "flex-direction")  valid = ParseKeyword(value, kDirectionNames, style.direction);
    else if (property == "justify-content") valid = ParseKeyword(value, kJustifyNames,   style.justify);
    else if (property == "align-items")     valid = ParseKeyword(value, kAlignNames,     style.align);
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
    else if (property == "caret-material")  valid = ParseName(value, values.caret_material);
    else if (property == "caret-quad")      valid = ParseName(value, values.caret_quad);
    else if (property == "caret-width")     valid = ParseCaretLength(value, style.caret_width_mode, style.caret_width);
    else if (property == "caret-gap")       valid = ParseCaretLength(value, style.caret_gap_mode, style.caret_gap);
    else return UIDeclarationResult::UnknownProperty;
    return valid ? UIDeclarationResult::Applied : UIDeclarationResult::InvalidValue;
}

static constexpr std::string_view kPropertyNames[] = {
    "display", "flex-direction", "justify-content", "align-items", "width", "height", "flex-grow", "gap",
    "padding-top", "padding-right", "padding-bottom", "padding-left",
    "margin-top", "margin-right", "margin-bottom", "margin-left",
    "material",
};
static_assert(std::size(kPropertyNames) == std::to_underlying(UIProperty::Count));

static std::string_view PropertyName(UIProperty property) { return kPropertyNames[std::to_underlying(property)]; }

static bool Covers(std::string_view declared, UIProperty property)
{
    const std::string_view name = PropertyName(property);
    return declared == name || (declared == "padding" && name.starts_with("padding-")) ||
           (declared == "margin" && name.starts_with("margin-"));
}

static std::string FormatNumber(float value)
{
    char buffer[32];
    // to_chars пишет -0 как "-0".
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value == 0.0f ? 0.0f : value);
    return std::string(buffer, result.ptr);
}

static std::string FormatLength(float px) { return FormatNumber(px) + "px"; }

static std::string FormatSize(UISize mode, float size)
{
    switch (mode) {
        case UISize::Points:  return FormatLength(size);
        case UISize::Percent: return FormatNumber(size) + "%";
        default:              return "auto";   // режим Grow в файле — flex-grow
    }
}

static std::string StyleValue(const UIStyle& style, UIProperty property)
{
    switch (property) {
        case UIProperty::Display:        return std::string(KeywordOf(kDisplayNames,   style.display));
        case UIProperty::FlexDirection:  return std::string(KeywordOf(kDirectionNames, style.direction));
        case UIProperty::JustifyContent: return std::string(KeywordOf(kJustifyNames,   style.justify));
        case UIProperty::AlignItems:     return std::string(KeywordOf(kAlignNames,     style.align));
        case UIProperty::Width:          return FormatSize(style.width_mode,  style.width);
        case UIProperty::Height:         return FormatSize(style.height_mode, style.height);
        case UIProperty::FlexGrow:       return FormatNumber(style.FlexGrow());
        case UIProperty::Gap:            return FormatLength(style.gap);
        case UIProperty::PaddingTop:     return FormatLength(style.padding.top);
        case UIProperty::PaddingRight:   return FormatLength(style.padding.right);
        case UIProperty::PaddingBottom:  return FormatLength(style.padding.bottom);
        case UIProperty::PaddingLeft:    return FormatLength(style.padding.left);
        case UIProperty::MarginTop:      return FormatLength(style.margin.top);
        case UIProperty::MarginRight:    return FormatLength(style.margin.right);
        case UIProperty::MarginBottom:   return FormatLength(style.margin.bottom);
        case UIProperty::MarginLeft:     return FormatLength(style.margin.left);
        default:                         return {};
    }
}

static std::string MaterialValue(const std::string& material) { return material.empty() ? "none" : material; }

// Значение встаёт в атрибут style как есть: кавычка закрыла бы атрибут, а ';' и скобки — объявление.
static bool IsWritableValue(std::string_view value)
{
    return !value.empty() && value.find_first_of("\"';{}") == std::string_view::npos;
}

static bool ParseUnsigned(std::string_view text, uint32_t& number)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    return !text.empty() && error == std::errc{} && end == text.data() + text.size();
}

static size_t NameLength(std::string_view text)
{
    size_t length = 0;
    while (length < text.size() && IsVarNameChar(text[length])) ++length;
    return length;
}

static bool HasVars(const std::vector<UITemplatePart>& parts)
{
    return std::ranges::any_of(parts, [](const UITemplatePart& part) { return bool(part.var); });
}

static bool HasSubstitutions(const std::vector<UITemplatePart>& parts)
{
    return std::ranges::any_of(parts, [](const UITemplatePart& part) { return part.var || part.loop >= 0; });
}

static bool HasBoundDeclarations(const UIElement& element)
{
    return std::ranges::any_of(element.declarations, [](const UIDeclaration& declaration) { return HasVars(declaration.value); });
}

static const UIDeclaration* WinningDeclaration(const std::vector<UIDeclaration>& declarations, UIProperty property)
{
    for (size_t i = declarations.size(); i-- > 0;)
        if (Covers(declarations[i].property, property)) return &declarations[i];
    return nullptr;
}

static uint32_t BoundPropertiesOf(const std::vector<UIDeclaration>& declarations)
{
    uint32_t bound = 0;
    for (uint8_t index = 0; index < std::to_underlying(UIProperty::Count); ++index) {
        const UIProperty property{ index };
        const UIDeclaration* winner = WinningDeclaration(declarations, property);
        if (winner && HasSubstitutions(winner->value)) bound |= UIPropertyBit(property);
    }
    return bound;
}

template <class ValueOf>
static std::string RenderTemplate(const std::vector<UITemplatePart>& parts, const ValueOf& value_of)
{
    std::string out;
    for (const UITemplatePart& part : parts) {
        out += part.literal;
        if (part.var || part.loop >= 0) out += value_of(part);
    }
    return out;
}

template <class ValueOf>
static UIDeclaredValues RenderValues(const std::vector<UIDeclaration>& declarations, const ValueOf& value_of,
                                     const std::string& path)
{
    UIDeclaredValues values;
    for (const UIDeclaration& declaration : declarations) {
        const std::string value = RenderTemplate(declaration.value, value_of);
        // Разбор пишет в поле до проверки диапазона: неверное значение применяется к копии.
        UIDeclaredValues candidate = values;
        if (ApplyDeclaration(declaration.property, value, candidate) == UIDeclarationResult::Applied)
            values = std::move(candidate);
        else
            SDL_Log("UI_Manager: %s:%u: invalid value '%s' for '%s'", path.c_str(), declaration.line, value.c_str(),
                    declaration.property.c_str());
    }
    return values;
}

template <class ValueOf>
static UIDeclaredValues RenderElement(const UIElement& element, const ValueOf& value_of, const std::string& path)
{
    UIDeclaredValues values = RenderValues(element.declarations, value_of, path);
    for (const UIEdit& edit : element.edits) ApplyDeclaration(PropertyName(edit.property), edit.value, values);
    return values;
}

static UIVarKey ResolveKey(const std::vector<UIKeyIndex>& key, const UIVarKey& instance)
{
    UIVarKey resolved;
    resolved.reserve(key.size());
    for (const UIKeyIndex& index : key) resolved.push_back(index.from_loop ? instance[index.value] : index.value);
    return resolved;
}

static bool ParseReference(UILoadState& state, std::string_view text, uint32_t line, UITemplatePart& part)
{
    text = TrimSpaces(text);
    const std::string source(text);
    const auto fail = [&state, line](const std::string& message) { state.Error(line, message); return false; };

    const std::string_view name = text.substr(0, NameLength(text));
    if (!IsVarName(name)) return fail("'" + source + "' must start with a variable or a loop index");
    text.remove_prefix(name.size());
    if (const int32_t depth = state.FindLoopIndex(name); depth >= 0) part.loop = depth;
    else if (const UILoadedVar* var = state.FindVar(name))         part.var  = var->id;
    else return fail("variable '" + std::string(name) + "' is not declared with <var>");

    for (text = TrimSpaces(text); !text.empty(); text = TrimSpaces(text)) {
        const size_t close = text.find(']');
        if (text.front() != '[' || close == std::string_view::npos) return fail("malformed index in '" + source + "'");
        const std::string_view index = TrimSpaces(text.substr(1, close - 1));
        text.remove_prefix(close + 1);
        UIKeyIndex key_index;
        if (const int32_t depth = state.FindLoopIndex(index); depth >= 0) key_index = { safe_i_u32(depth), true };
        else if (!ParseUnsigned(index, key_index.value))
            return fail("index '" + std::string(index) + "' in '" + source + "' must be a loop index or a number");
        part.key.push_back(key_index);
    }
    if (part.loop >= 0 && !part.key.empty()) return fail("loop index '" + std::string(name) + "' takes no [ ]");
    return true;
}

// Часть шаблона — текст и следом за ним подстановка; у последней части подстановки нет.
static bool ParseTemplate(UILoadState& state, std::string_view source, uint32_t line, std::vector<UITemplatePart>& parts)
{
    parts.assign(1, UITemplatePart{});
    while (true) {
        const size_t open = source.find("{{");
        if (open == std::string_view::npos) { parts.back().literal += source; return true; }
        parts.back().literal += source.substr(0, open);
        const size_t close = source.find("}}", open + 2);
        if (close == std::string_view::npos) { state.Error(line, "'{{' is not closed with '}}'"); return false; }
        if (!ParseReference(state, source.substr(open + 2, close - open - 2), line, parts.back())) return false;
        parts.emplace_back();
        source.remove_prefix(close + 2);
    }
}

static bool ParseLoopHeader(UILoadState& state, const UIMarkupNode& markup, std::string& index_name,
                            std::vector<UITemplatePart>& count)
{
    std::string_view rest = markup.header;
    const auto take_word = [&rest] {
        rest = TrimSpaces(rest);
        const std::string_view word = rest.substr(0, NameLength(rest));
        rest.remove_prefix(word.size());
        return word;
    };
    const std::string_view name    = take_word();
    const std::string_view keyword = take_word();
    const std::string_view range   = take_word();
    rest = TrimSpaces(rest);
    if (!IsVarName(name) || keyword != "in" || range != "range" || !rest.starts_with('(') || !rest.ends_with(')')) {
        state.Error(markup.line, "<for> takes 'NAME in range(COUNT)', not '" + markup.header + "'");
        return false;
    }

    index_name = name;
    if (index_name != "_" && state.FindVar(index_name)) {
        state.Error(markup.line, "loop index '" + index_name + "' hides variable '" + index_name + "'");
        return false;
    }
    if (state.FindLoopIndex(index_name) >= 0) {
        state.Error(markup.line, "loop index '" + index_name + "' hides the index of an outer loop");
        return false;
    }

    const std::string_view operand = TrimSpaces(rest.substr(1, rest.size() - 2));
    count.assign(1, UITemplatePart{});
    uint32_t number = 0;
    if (ParseUnsigned(operand, number)) { count[0].literal = operand; return true; }
    if (!ParseReference(state, operand, markup.line, count[0])) return false;
    count.emplace_back();
    return true;
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
    if (const UIMarkupAttribute* default_value = element.Attribute("default")) var.default_value = default_value->value;
    return true;
}

static void LoadDeclarations(UILoadState& state, const UIMarkupNode& markup, UIElement& element, UIDeclaredValues& values,
                             bool& material_bound)
{
    const auto value_of = [&state](const UITemplatePart& part) { return state.ValueOf(part); };
    const std::vector<const UIMarkupDeclaration*> resolved = ResolveUIStyle(state.document, markup);
    // ResolveUIStyle кладёт объявления атрибута style последними.
    const size_t first_from_style_attribute = resolved.size() - markup.style.size();
    for (size_t index = 0; index < resolved.size(); ++index) {
        const UIMarkupDeclaration* declaration = resolved[index];
        UIDeclaration& entry = element.declarations.emplace_back(UIDeclaration{
            declaration->property, {}, declaration->line, declaration->span, index >= first_from_style_attribute });
        if (!ParseTemplate(state, declaration->value, declaration->line, entry.value)) continue;
        const bool substituted = HasSubstitutions(entry.value);
        if (substituted && (entry.property == "quad" || entry.property == "font-family" ||
                            entry.property == "caret-material" || entry.property == "caret-quad")) {
            state.Error(declaration->line, "'" + entry.property + "' takes no {{ }}: it is read once at load");
            continue;
        }
        material_bound = material_bound || (substituted && entry.property == "material");

        const std::string value = RenderTemplate(entry.value, value_of);
        switch (ApplyDeclaration(entry.property, value, values)) {
            case UIDeclarationResult::Applied:
                break;
            case UIDeclarationResult::UnknownProperty:
                state.Error(declaration->line, "unknown property '" + entry.property + "'");
                break;
            case UIDeclarationResult::InvalidValue:
                state.Error(declaration->line, "invalid value '" + value + "' for '" + entry.property + "'" +
                                               (substituted ? " from '" + declaration->value + "'" : ""));
                break;
        }
    }
}

static void LoadLoop(UILoadState& state, const UIMarkupNode& markup, int32_t parent);

static void LoadElement(UILoadState& state, const UIMarkupNode& markup, int32_t parent)
{
    UIElement element;
    element.line = markup.line;
    if (parent < 0) {
        if (markup.tag != "ui") { state.Error(markup.line, "root element must be <ui>, not <" + markup.tag + ">"); return; }
        element.kind = UIElementKind::Root;
    }
    else if (markup.tag == "box")  element.kind = UIElementKind::Box;
    else if (markup.tag == "text") element.kind = UIElementKind::Text;
    else if (markup.tag == "input") {
        element.kind  = UIElementKind::Text;
        element.input = true;
    }
    else if (markup.tag == "for")  { LoadLoop(state, markup, parent); return; }
    else if (markup.tag == "var") {
        if (state.elements[safe_i_u32(parent)].kind != UIElementKind::Root)
            state.Error(markup.line, "<var> belongs directly in <ui>");
        return;
    }
    else if (markup.tag == "br") { state.Error(markup.line, "<br/> belongs in <text> or <input>"); return; }
    else { state.Error(markup.line, "unknown tag <" + markup.tag + ">"); return; }

    for (const UIMarkupAttribute& attribute : markup.attributes) {
        if (attribute.name != "id" && attribute.name != "class")
            state.Error(attribute.line, "attribute '" + attribute.name + "' is not supported on <" + markup.tag + ">");
        else if (attribute.value.find("{{") != std::string::npos)
            state.Error(attribute.line, "attribute '" + attribute.name + "' takes no {{ }}");
    }
    if (const UIMarkupAttribute* id = markup.Attribute("id")) element.id = id->value;
    if (const UIMarkupAttribute* classes = markup.Attribute("class")) element.classes = SplitClasses(classes->value);
    element.style_value    = markup.style_value;
    element.attributes_end = markup.attributes_end;

    UIDeclaredValues values;
    bool material_bound = false;
    LoadDeclarations(state, markup, element, values, material_bound);
    element.bound_properties = BoundPropertiesOf(element.declarations);

    if (element.kind == UIElementKind::Text) {
        for (const UIMarkupNode& child : markup.children)
            if (!child.IsText() && child.tag != "br")
                state.Error(child.line, "<" + markup.tag + "> holds only text and <br/>, not <" + child.tag + ">");
        ParseTemplate(state, UIMarkupText(markup), markup.line, element.text);
        if (element.input) {
            if (std::ranges::count_if(element.text, [](const UITemplatePart& part) { return bool(part.var); }) != 1)
                state.Error(markup.line, "<input> needs exactly one {{variable}}");
            if (values.caret_material.empty()) state.Error(markup.line, "<input> needs caret-material");
            if (values.caret_quad.empty())     state.Error(markup.line, "<input> needs caret-quad");
        }
        if (values.font_family.empty())
            state.Error(markup.line, "<" + markup.tag + "> needs font-family");
        else if (!state.font_manager->FontIdOf(values.font_family))
            state.Error(markup.line, "font '" + values.font_family + "' is not created");
        if (values.material.empty()) state.Error(markup.line, "<" + markup.tag + "> needs material");
        if (values.quad.empty())     state.Error(markup.line, "<" + markup.tag + "> needs quad");
    }
    else if (element.kind == UIElementKind::Box && (!values.material.empty() || material_bound) && values.quad.empty())
        state.Error(markup.line, "<box> with material needs quad");

    const int32_t index = safe_size_i(state.elements.size());
    if (parent >= 0) state.elements[safe_i_u32(parent)].children.push_back(safe_i_u32(index));
    const bool holds_elements = element.kind != UIElementKind::Text;
    state.elements.push_back(std::move(element));
    if (!holds_elements) return;
    for (const UIMarkupNode& child : markup.children) {
        if (child.IsText()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, index);
    }
}

static void LoadLoop(UILoadState& state, const UIMarkupNode& markup, int32_t parent)
{
    UIElement element;
    element.kind = UIElementKind::Loop;
    element.line = markup.line;
    std::string index_name;
    if (ParseLoopHeader(state, markup, index_name, element.count)) {
        const std::string count = RenderTemplate(element.count, [&state](const UITemplatePart& part) { return state.ValueOf(part); });
        uint32_t number = 0;
        if (!ParseUnsigned(count, number))
            state.Error(markup.line, "invalid loop count '" + count + "' from '" + markup.header + "'");
    }

    const int32_t index = safe_size_i(state.elements.size());
    state.elements[safe_i_u32(parent)].children.push_back(safe_i_u32(index));
    state.elements.push_back(std::move(element));

    state.loop_indices.push_back(index_name == "_" ? std::string() : index_name);
    for (const UIMarkupNode& child : markup.children) {
        if (child.IsText()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, index);
    }
    state.loop_indices.pop_back();
}

static bool ReadTextFile(const std::string& path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

// Запись идёт во временный файл, который затем подменяет исходник: оборванная запись не испортит рукописный файл.
static bool WriteTextFile(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(text.data(), safe_size_ss(text.size()));
    file.close();
    if (!file) return false;
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool UI_Manager::Load(const FontManager* font_manager, const std::string& path)
{
    std::string source;
    if (!ReadTextFile(path, source)) { SDL_Log("UI_Manager::Load: cannot open '%s'", path.c_str()); return false; }
    if (!LoadSource(font_manager, path, source)) return false;
    disk_source = std::move(source);
    return true;
}

bool UI_Manager::Save(const FontManager* font_manager)
{
    if (document_path.empty()) { SDL_Log("UI_Manager::Save: no UI file is loaded"); return false; }
    if (!HasUnsavedChanges()) return true;
    // LoadSource через Root чистит document_path.
    const std::string path = document_path;
    std::string disk;
    if (!ReadTextFile(path, disk)) { SDL_Log("UI_Manager::Save: cannot read '%s'", path.c_str()); return false; }
    if (disk != disk_source) {
        SDL_Log("UI_Manager::Save: '%s' was changed outside the editor; Load picks it up and drops the edits", path.c_str());
        return false;
    }

    if (unsaved_edits > 0) {
        if (!LoadSource(font_manager, path, SourceWithEdits())) {
            SDL_Log("UI_Manager::Save: the edited file does not load, '%s' is not written", path.c_str());
            return false;
        }
        source_unsaved = true;
    }
    if (!WriteTextFile(path, document_source)) { SDL_Log("UI_Manager::Save: cannot write '%s'", path.c_str()); return false; }
    disk_source    = document_source;
    source_unsaved = false;
    return true;
}

bool UI_Manager::LoadSource(const FontManager* font_manager, const std::string& path, const std::string& source)
{
    if (!font_manager) { SDL_Log("UI_Manager::Load: no FontManager"); return false; }
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
        const UIVar* existing = variables.Get(var.id);
        const auto scalar = existing->elements.find(UIVarKey{});
        const bool assigned = scalar != existing->elements.end() && scalar->second.assigned;
        var.value = assigned ? scalar->second.value : var.default_value;
        state.vars.push_back(std::move(var));
    }
    LoadElement(state, document->root, -1);
    if (!state.errors.empty()) {
        for (const std::string& error : state.errors) SDL_Log("UI_Manager::Load: %s", error.c_str());
        return false;
    }

    for (const UILoadedVar& var : state.vars) variables.Get(var.id)->default_value = var.default_value;

    const UIVarKey no_instance;
    const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
    const Node root_node = Root(RenderValues(state.elements[0].declarations, value_of, path).style);
    elements        = std::move(state.elements);
    document_path   = path;
    document_source = source;
    nodes[root_node].id      = elements[0].id;
    nodes[root_node].classes = elements[0].classes;
    nodes[root_node].element = 0;
    if (HasBoundDeclarations(elements[0])) AddBinding({ UIBindingKind::Style, root_node, 0 });
    std::vector<UISlot> slots = BuildItems(0, root_node, no_instance, font_manager);
    nodes[root_node].slots = std::move(slots);
    return true;
}

std::string UI_Manager::PartValue(const UITemplatePart& part, const UIVarKey& instance) const
{
    if (part.loop >= 0) return std::to_string(instance[safe_i_u32(part.loop)]);
    return GetVar(part.var, ResolveKey(part.key, instance));
}

bool UI_Manager::IsInputNode(Node node) const
{
    return node < nodes.size() && nodes[node].element < elements.size() && elements[nodes[node].element].input;
}

bool UI_Manager::InputVarOf(Node node, UIVarId& var, UIVarKey& key) const
{
    if (!IsInputNode(node)) return false;
    const std::vector<UITemplatePart>& text = elements[nodes[node].element].text;
    const auto part = std::ranges::find_if(text, [](const UITemplatePart& candidate) { return bool(candidate.var); });
    var = part->var;
    key = ResolveKey(part->key, nodes[node].instance);
    return true;
}

std::vector<UISlot> UI_Manager::BuildItems(uint32_t element, Node parent, const UIVarKey& instance,
                                           const FontManager* font_manager)
{
    std::vector<UISlot> slots;
    for (uint32_t child : elements[element].children) {
        if (elements[child].kind == UIElementKind::Loop) {
            slots.push_back({ .loop = BuildLoop(child, parent, instance, font_manager) });
            continue;
        }
        const Node node = BuildNode(child, parent, instance, font_manager);
        slots.push_back({ .node = node });
        std::vector<UISlot> child_slots = BuildItems(child, node, instance, font_manager);
        nodes[node].slots = std::move(child_slots);
    }
    return slots;
}

UI_Manager::Node UI_Manager::BuildNode(uint32_t element_index, Node parent, const UIVarKey& instance,
                                       const FontManager* font_manager)
{
    const UIElement& element = elements[element_index];
    const auto value_of = [this, &instance](const UITemplatePart& part) { return PartValue(part, instance); };
    UIDeclaredValues values = RenderElement(element, value_of, document_path);
    Node node = kInvalid;
    if (element.kind == UIElementKind::Text) {
        values.style.padding = {};
        node = Text(parent, values.style, RenderTemplate(element.text, value_of), values.material, values.quad,
                    font_manager->FontIdOf(values.font_family), font_manager);
    }
    else
        node = Box(parent, values.style, values.material, values.quad);
    nodes[node].id             = element.id;
    nodes[node].classes        = element.classes;
    nodes[node].element        = element_index;
    nodes[node].instance       = instance;
    nodes[node].caret_material = values.caret_material;
    nodes[node].caret_quad     = values.caret_quad;

    if (HasVars(element.text))           AddBinding({ UIBindingKind::Text,  node, element_index, instance });
    if (HasBoundDeclarations(element))   AddBinding({ UIBindingKind::Style, node, element_index, instance });
    return node;
}

uint32_t UI_Manager::BuildLoop(uint32_t element, Node parent, const UIVarKey& outer, const FontManager* font_manager)
{
    const uint32_t loop = safe_u32(loops.size());
    loops.push_back(UILoop{ element, parent, outer });
    if (HasVars(elements[element].count)) AddBinding({ UIBindingKind::Count, loop, element, outer });
    uint32_t count = 0;
    if (ReadLoopCount(loop, count)) loops[loop].count = count;
    BuildInstances(loop, loops[loop].count, font_manager);
    return loop;
}

void UI_Manager::BuildInstances(uint32_t loop, uint32_t count, const FontManager* font_manager)
{
    while (loops[loop].instances.size() < count) {
        UIVarKey instance = loops[loop].outer;
        instance.push_back(safe_u32(loops[loop].instances.size()));
        std::vector<UISlot> slots = BuildItems(loops[loop].element, loops[loop].parent, instance, font_manager);
        loops[loop].instances.push_back({ std::move(slots) });
    }
}

bool UI_Manager::ReadLoopCount(uint32_t loop, uint32_t& count) const
{
    const UIElement& element = elements[loops[loop].element];
    const UIVarKey&  outer   = loops[loop].outer;
    const std::string value = RenderTemplate(element.count, [this, &outer](const UITemplatePart& part) { return PartValue(part, outer); });
    if (ParseUnsigned(value, count)) return true;
    SDL_Log("UI_Manager: %s:%u: invalid loop count '%s'", document_path.c_str(), element.line, value.c_str());
    return false;
}

void UI_Manager::ResizeLoop(uint32_t loop, uint32_t count, const FontManager* font_manager)
{
    loops[loop].count = count;
    if (count > loops[loop].instances.size()) {
        BuildInstances(loop, count, font_manager);
        RebuildChildren(loops[loop].parent);
    }
    UpdateParking(loop);
}

void UI_Manager::UpdateParking(uint32_t loop)
{
    for (uint32_t index = 0; index < loops[loop].instances.size(); ++index) {
        const bool parked = loops[loop].parked || index >= loops[loop].count;
        for (const UISlot& slot : loops[loop].instances[index].slots) {
            if (slot.node != kInvalid) { SetNodeParked(slot.node, parked); continue; }
            loops[slot.loop].parked = parked;
            UpdateParking(slot.loop);
        }
    }
}

void UI_Manager::AppendSlotNodes(const std::vector<UISlot>& slots, std::vector<Node>& out) const
{
    for (const UISlot& slot : slots) {
        if (slot.node != kInvalid) { out.push_back(slot.node); continue; }
        for (const UILoopInstance& instance : loops[slot.loop].instances) AppendSlotNodes(instance.slots, out);
    }
}

void UI_Manager::AddBinding(UIBinding binding)
{
    const uint32_t index = safe_u32(bindings.size());
    const auto link = [this, index, &binding](const std::vector<UITemplatePart>& parts) {
        for (const UITemplatePart& part : parts) {
            UIVar* var = variables.Get(part.var);
            if (!var) continue;
            std::vector<uint32_t>& linked = var->elements[ResolveKey(part.key, binding.instance)].bindings;
            if (linked.empty() || linked.back() != index) linked.push_back(index);
        }
    };
    const UIElement& element = elements[binding.element];
    switch (binding.kind) {
        case UIBindingKind::Text:  link(element.text); break;
        case UIBindingKind::Style: for (const UIDeclaration& declaration : element.declarations) link(declaration.value); break;
        case UIBindingKind::Count: link(element.count); break;
    }
    bindings.push_back(std::move(binding));
}

UIVarId UI_Manager::InternVar(const std::string& name)
{
    const UIVarId id = variables.Intern(name);
    if (id && !variables.Get(id)) variables.Put(id, std::make_unique<UIVar>());
    return id;
}

std::string UI_Manager::GetVar(UIVarId id, const UIVarKey& key) const
{
    const UIVar* var = variables.Get(id);
    if (!var) return {};
    const auto element = var->elements.find(key);
    return element != var->elements.end() && element->second.assigned ? element->second.value : var->default_value;
}

void UI_Manager::SetVar(UIVarId id, const UIVarKey& key, std::string_view value)
{
    UIVar* var = variables.Get(id);
    if (!var) return;
    UIVarElement& element = var->elements[key];
    const bool unchanged = (element.assigned ? element.value : var->default_value) == value;
    element.assigned = true;
    element.value    = value;
    if (unchanged) return;
    for (uint32_t index : element.bindings) {
        UIBinding& binding = bindings[index];
        if (binding.queued) continue;
        binding.queued = true;
        dirty_bindings.push_back(index);
    }
}

void UI_Manager::ApplyDirtyBindings(EngineContext* ctx, const FontManager* font_manager, std::mutex& structure_mutex)
{
    // Рост цикла сверх запаса добавляет узлы, а панели редактора ходят по ним с рендер-потока: рост идёт под
    // тем же мьютексом, что и загрузка файла.
    std::unique_lock<std::mutex> structure_lock(structure_mutex, std::defer_lock);
    for (size_t i = 0; i < dirty_bindings.size(); ++i) {
        bindings[dirty_bindings[i]].queued = false;
        // Копия: постройка экземпляров добавляет связи, и вектор связей переезжает.
        const UIBinding  binding = bindings[dirty_bindings[i]];
        const UIElement& element = elements[binding.element];
        const auto value_of = [this, &binding](const UITemplatePart& part) { return PartValue(part, binding.instance); };
        switch (binding.kind) {
            case UIBindingKind::Text:
                SetText(binding.target, RenderTemplate(element.text, value_of), font_manager);
                break;
            case UIBindingKind::Style: {
                // Стиль собирается заново из всех объявлений узла: связанное объявление может перекрываться
                // следующими, в том числе частично (padding и padding-left).
                const UIDeclaredValues values = RenderElement(element, value_of, document_path);
                SetNodeStyle(binding.target, values.style);
                SetNodeMaterial(ctx, binding.target, values.material);
                break;
            }
            case UIBindingKind::Count: {
                uint32_t count = 0;
                if (!ReadLoopCount(binding.target, count)) break;
                if (count > loops[binding.target].instances.size() && !structure_lock.owns_lock()) structure_lock.lock();
                ResizeLoop(binding.target, count, font_manager);
                break;
            }
        }
    }
    dirty_bindings.clear();
}

// У экземпляров цикла один элемент на всех: правку одного экземпляра файл не выражает.
UIElement* UI_Manager::EditableElement(Node node)
{
    if (node >= nodes.size() || nodes[node].element >= elements.size() || !nodes[node].instance.empty()) return nullptr;
    return &elements[nodes[node].element];
}

void UI_Manager::SetEdit(UIElement& element, UIProperty property, std::string value, const std::string& file_value)
{
    const auto edit = std::ranges::find(element.edits, property, &UIEdit::property);
    if (value == file_value) {
        if (edit == element.edits.end()) return;
        element.edits.erase(edit);
        --unsaved_edits;
        return;
    }
    if (edit != element.edits.end()) { edit->value = std::move(value); return; }
    element.edits.push_back({ property, std::move(value) });
    ++unsaved_edits;
}

void UI_Manager::EditNodeStyle(Node node, const UIStyle& style)
{
    if (node >= nodes.size()) return;
    if (UIElement* element = EditableElement(node)) {
        const UIVarKey no_instance;
        const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
        const UIStyle file_style = RenderValues(element->declarations, value_of, document_path).style;
        for (uint8_t index = 0; index < std::to_underlying(UIProperty::Count); ++index) {
            const UIProperty property{ index };
            if (property == UIProperty::Material || (element->bound_properties & UIPropertyBit(property))) continue;
            std::string value = StyleValue(style, property);
            if (value != StyleValue(nodes[node].style, property))
                SetEdit(*element, property, std::move(value), StyleValue(file_style, property));
        }
    }
    SetNodeStyle(node, style);
}

void UI_Manager::EditNodeMaterial(EngineContext* ctx, Node node, const std::string& material)
{
    if (node >= nodes.size() || (nodes[node].shape && material.empty())) return;
    UIElement* element = EditableElement(node);
    if (element && !(element->bound_properties & UIPropertyBit(UIProperty::Material))) {
        const UIVarKey no_instance;
        const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
        std::string value = MaterialValue(material);
        if (IsWritableValue(value))
            SetEdit(*element, UIProperty::Material, std::move(value),
                    MaterialValue(RenderValues(element->declarations, value_of, document_path).material));
        else
            SDL_Log("UI_Manager: material '%s' cannot be written to the UI file", material.c_str());
    }
    SetNodeMaterial(ctx, node, material);
}

struct UISourcePatch {
    uint32_t    offset = 0;
    uint32_t    length = 0;
    std::string text;
};

// Правка заменяет объявление атрибута style, если оно и решает значение свойства, а иначе дописывается в конец
// атрибута: он последний в каскаде и перекрывает любые правила. Правила <style> не меняются.
std::string UI_Manager::SourceWithEdits() const
{
    std::vector<UISourcePatch> patches;
    for (const UIElement& element : elements) {
        std::string appended;
        for (const UIEdit& edit : element.edits) {
            const std::string_view property = PropertyName(edit.property);
            const UIDeclaration* winner = WinningDeclaration(element.declarations, edit.property);
            if (winner && winner->from_style_attribute && winner->property == property && !HasSubstitutions(winner->value)) {
                patches.push_back({ winner->span.offset, winner->span.length, edit.value });
                continue;
            }
            if (!appended.empty()) appended += "; ";
            appended.append(property).append(": ").append(edit.value);
        }
        if (appended.empty()) continue;

        if (!element.style_value) {
            patches.push_back({ element.attributes_end, 0, " style=\"" + appended + "\"" });
            continue;
        }
        const UIMarkupSpan span = *element.style_value;
        const std::string_view style = std::string_view(document_source).substr(span.offset, span.length);
        const size_t last = style.find_last_not_of(" \t\r\n\f");
        if (last == std::string_view::npos) { patches.push_back({ span.offset, 0, appended }); continue; }
        patches.push_back({ span.offset + safe_u32(last + 1), 0, (style[last] == ';' ? " " : "; ") + appended });
    }

    // С конца файла: замена другой длины сдвигает всё, что за ней.
    std::ranges::sort(patches, std::ranges::greater{}, &UISourcePatch::offset);
    std::string source = document_source;
    for (const UISourcePatch& patch : patches) source.replace(patch.offset, patch.length, patch.text);
    return source;
}
