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
    UIStyle          style;
    std::string      material, quad, font_family, caret_material, caret_quad, thumb_quad;
    UIThumbMaterials thumb_materials;
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

    std::optional<UILoopIndexReference> FindLoopIndex(std::string_view name) const
    {
        for (size_t depth = loop_indices.size(); depth-- > 0;)
            if (loop_indices[depth] == name) return UILoopIndexReference{ safe_u32(depth) };
        return std::nullopt;
    }

    std::string ValueOf(const UITemplatePart& part) const
    {
        if (const std::string* literal = std::get_if<std::string>(&part)) return *literal;
        if (const UIVarReference* reference = std::get_if<UIVarReference>(&part)) {
            for (const UILoadedVar& var : vars)
                if (var.id == reference->var) return reference->key.empty() ? var.value : var.default_value;
            return {};
        }
        if (std::holds_alternative<UILoopIndexReference>(part)) return "0";
        assert(false && "UILoadState::ValueOf: template part of unknown kind");
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

static constexpr std::string_view kNoMinimum = "auto";
static constexpr std::string_view kNoMaximum = "none";

static bool ParseLimit(std::string_view value, std::string_view no_limit, UILimit& mode, float& limit)
{
    if (value == no_limit) { mode = UILimit::None; limit = 0.0f; return true; }
    UISize size_mode = UISize::Auto;
    if (!ParseSize(value, size_mode, limit) || size_mode == UISize::Auto) return false;
    mode = size_mode == UISize::Percent ? UILimit::Percent : UILimit::Points;
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
static constexpr UIKeyword<UIOverflow> kOverflowNames[] = {
    { "visible", UIOverflow::Visible }, { "hidden", UIOverflow::Hidden }, { "scroll", UIOverflow::Scroll },
    { "auto", UIOverflow::Auto },
};
static constexpr UIKeyword<UIAxis> kAxisNames[] = { { "x", UIAxis::X }, { "y", UIAxis::Y } };

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

static bool ParseOverflow(std::string_view value, UIOverflow& overflow_x, UIOverflow& overflow_y)
{
    UIOverflow overflows[2] = {};
    size_t count = 0;
    while (true) {
        const size_t begin = value.find_first_not_of(" \t\r\n\f");
        if (begin == std::string_view::npos) break;
        value.remove_prefix(begin);
        const size_t end = std::min(value.find_first_of(" \t\r\n\f"), value.size());
        if (count == 2 || !ParseKeyword(value.substr(0, end), kOverflowNames, overflows[count])) return false;
        ++count;
        value.remove_prefix(end);
    }
    if (count == 0) return false;
    overflow_x = overflows[0];
    overflow_y = overflows[count - 1];
    return true;
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
    else if (property == "min-width")       valid = ParseLimit(value, kNoMinimum, style.min_width_mode,  style.min_width);
    else if (property == "max-width")       valid = ParseLimit(value, kNoMaximum, style.max_width_mode,  style.max_width);
    else if (property == "min-height")      valid = ParseLimit(value, kNoMinimum, style.min_height_mode, style.min_height);
    else if (property == "max-height")      valid = ParseLimit(value, kNoMaximum, style.max_height_mode, style.max_height);
    else if (property == "flex-grow")       valid = ParseFloat(value, style.grow) && style.grow >= 0.0f;
    else if (property == "flex-shrink")     valid = ParseFloat(value, style.shrink) && style.shrink >= 0.0f;
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
    else if (property == "overflow")        valid = ParseOverflow(value, style.overflow_x, style.overflow_y);
    else if (property == "overflow-x")      valid = ParseKeyword(value, kOverflowNames, style.overflow_x);
    else if (property == "overflow-y")      valid = ParseKeyword(value, kOverflowNames, style.overflow_y);
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
    else if (property == "thumb-material")        valid = ParseName(value, values.thumb_materials.idle);
    else if (property == "thumb-hover-material")  valid = ParseName(value, values.thumb_materials.hovered);
    else if (property == "thumb-active-material") valid = ParseName(value, values.thumb_materials.grabbed);
    else if (property == "thumb-quad")            valid = ParseName(value, values.thumb_quad);
    else return UIDeclarationResult::UnknownProperty;
    return valid ? UIDeclarationResult::Applied : UIDeclarationResult::InvalidValue;
}

static constexpr std::string_view kPropertyNames[] = {
    "display", "flex-direction", "justify-content", "align-items", "width", "height",
    "min-width", "max-width", "min-height", "max-height", "flex-grow", "flex-shrink", "gap",
    "padding-top", "padding-right", "padding-bottom", "padding-left",
    "margin-top", "margin-right", "margin-bottom", "margin-left",
    "overflow-x", "overflow-y",
    "material",
};
static_assert(std::size(kPropertyNames) == std::to_underlying(UIProperty::Count));

static std::string_view PropertyName(UIProperty property) { return kPropertyNames[std::to_underlying(property)]; }

static bool Covers(std::string_view declared, UIProperty property)
{
    const std::string_view name = PropertyName(property);
    return declared == name || (declared == "padding" && name.starts_with("padding-")) ||
           (declared == "margin" && name.starts_with("margin-")) || (declared == "overflow" && name.starts_with("overflow-"));
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

static std::string FormatLimit(UILimit mode, float limit, std::string_view no_limit)
{
    switch (mode) {
        case UILimit::Points:  return FormatLength(limit);
        case UILimit::Percent: return FormatNumber(limit) + "%";
        default:               return std::string(no_limit);
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
        case UIProperty::MinWidth:       return FormatLimit(style.min_width_mode,  style.min_width,  kNoMinimum);
        case UIProperty::MaxWidth:       return FormatLimit(style.max_width_mode,  style.max_width,  kNoMaximum);
        case UIProperty::MinHeight:      return FormatLimit(style.min_height_mode, style.min_height, kNoMinimum);
        case UIProperty::MaxHeight:      return FormatLimit(style.max_height_mode, style.max_height, kNoMaximum);
        case UIProperty::FlexGrow:       return FormatNumber(style.FlexGrow());
        case UIProperty::FlexShrink:     return FormatNumber(style.shrink);
        case UIProperty::Gap:            return FormatLength(style.gap);
        case UIProperty::PaddingTop:     return FormatLength(style.padding.top);
        case UIProperty::PaddingRight:   return FormatLength(style.padding.right);
        case UIProperty::PaddingBottom:  return FormatLength(style.padding.bottom);
        case UIProperty::PaddingLeft:    return FormatLength(style.padding.left);
        case UIProperty::MarginTop:      return FormatLength(style.margin.top);
        case UIProperty::MarginRight:    return FormatLength(style.margin.right);
        case UIProperty::MarginBottom:   return FormatLength(style.margin.bottom);
        case UIProperty::MarginLeft:     return FormatLength(style.margin.left);
        case UIProperty::OverflowX:      return std::string(KeywordOf(kOverflowNames, style.overflow_x));
        case UIProperty::OverflowY:      return std::string(KeywordOf(kOverflowNames, style.overflow_y));
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

static bool IsVarReference(const UITemplatePart& part) { return std::holds_alternative<UIVarReference>(part); }

static bool HasVars(const std::vector<UITemplatePart>& parts)
{
    return std::ranges::any_of(parts, IsVarReference);
}

static bool HasSubstitutions(const std::vector<UITemplatePart>& parts)
{
    return std::ranges::any_of(parts, [](const UITemplatePart& part) { return !std::holds_alternative<std::string>(part); });
}

static bool HasBoundDeclarations(const UIElementStyle& style)
{
    return std::ranges::any_of(style.declarations, [](const UIDeclaration& declaration) { return HasVars(declaration.value); });
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
    for (const UITemplatePart& part : parts) out += value_of(part);
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
static UIDeclaredValues RenderStyle(const UIElementStyle& style, const ValueOf& value_of, const std::string& path)
{
    UIDeclaredValues values = RenderValues(style.declarations, value_of, path);
    for (const UIEdit& edit : style.edits) ApplyDeclaration(PropertyName(edit.property), edit.value, values);
    return values;
}

static UIVarKey ResolveKey(const std::vector<UIKeyIndex>& key, const UIVarKey& instance)
{
    UIVarKey resolved;
    resolved.reserve(key.size());
    for (const UIKeyIndex& index : key) {
        if (const uint32_t* number = std::get_if<uint32_t>(&index))
            resolved.push_back(*number);
        else if (const UILoopIndexReference* loop_index = std::get_if<UILoopIndexReference>(&index))
            resolved.push_back(instance[loop_index->depth]);
        else
            assert(false && "ResolveKey: key index of unknown kind");
    }
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
    const std::optional<UILoopIndexReference> loop_index = state.FindLoopIndex(name);
    const UILoadedVar* var = loop_index ? nullptr : state.FindVar(name);
    if (!loop_index && !var) return fail("variable '" + std::string(name) + "' is not declared with <var>");

    std::vector<UIKeyIndex> key;
    for (text = TrimSpaces(text); !text.empty(); text = TrimSpaces(text)) {
        const size_t close = text.find(']');
        if (text.front() != '[' || close == std::string_view::npos) return fail("malformed index in '" + source + "'");
        const std::string_view index = TrimSpaces(text.substr(1, close - 1));
        text.remove_prefix(close + 1);
        uint32_t number = 0;
        if (const std::optional<UILoopIndexReference> key_loop_index = state.FindLoopIndex(index))
            key.push_back(*key_loop_index);
        else if (ParseUnsigned(index, number))
            key.push_back(number);
        else
            return fail("index '" + std::string(index) + "' in '" + source + "' must be a loop index or a number");
    }

    if (loop_index) {
        if (!key.empty()) return fail("loop index '" + std::string(name) + "' takes no [ ]");
        part = *loop_index;
    }
    else
        part = UIVarReference{ var->id, std::move(key) };
    return true;
}

static bool ParseTemplate(UILoadState& state, std::string_view source, uint32_t line, std::vector<UITemplatePart>& parts)
{
    parts.clear();
    while (true) {
        const size_t open = source.find("{{");
        const std::string_view literal = source.substr(0, open);
        if (!literal.empty()) parts.push_back(std::string(literal));
        if (open == std::string_view::npos) return true;
        const size_t close = source.find("}}", open + 2);
        if (close == std::string_view::npos) { state.Error(line, "'{{' is not closed with '}}'"); return false; }
        UITemplatePart reference;
        if (!ParseReference(state, source.substr(open + 2, close - open - 2), line, reference)) return false;
        parts.push_back(std::move(reference));
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
    if (state.FindLoopIndex(index_name)) {
        state.Error(markup.line, "loop index '" + index_name + "' hides the index of an outer loop");
        return false;
    }

    const std::string_view operand = TrimSpaces(rest.substr(1, rest.size() - 2));
    count.clear();
    uint32_t number = 0;
    if (ParseUnsigned(operand, number)) { count.push_back(std::string(operand)); return true; }
    UITemplatePart reference;
    if (!ParseReference(state, operand, markup.line, reference)) return false;
    count.push_back(std::move(reference));
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

static void LoadDeclarations(UILoadState& state, const UIMarkupNode& markup, UIElementStyle& style, UIDeclaredValues& values,
                             bool& material_bound)
{
    const auto value_of = [&state](const UITemplatePart& part) { return state.ValueOf(part); };
    const std::vector<const UIMarkupDeclaration*> resolved = ResolveUIStyle(state.document, markup);
    // ResolveUIStyle кладёт объявления атрибута style последними.
    const size_t first_from_style_attribute = resolved.size() - markup.style.size();
    for (size_t index = 0; index < resolved.size(); ++index) {
        const UIMarkupDeclaration* declaration = resolved[index];
        UIDeclaration& entry = style.declarations.emplace_back(UIDeclaration{
            declaration->property, {}, declaration->line, declaration->span, index >= first_from_style_attribute });
        if (!ParseTemplate(state, declaration->value, declaration->line, entry.value)) continue;
        const bool substituted = HasSubstitutions(entry.value);
        if (substituted && (entry.property == "quad" || entry.property == "font-family" ||
                            entry.property == "caret-material" || entry.property == "caret-quad" ||
                            entry.property == "thumb-material" || entry.property == "thumb-hover-material" ||
                            entry.property == "thumb-active-material" || entry.property == "thumb-quad")) {
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

static void LoadLoop(UILoadState& state, const UIMarkupNode& markup, UIElementIndex parent);

static void AddChildElement(UILoadState& state, UIElementIndex parent, UIElementIndex child)
{
    std::vector<UIElementIndex>* children = state.elements[parent.value].Children();
    assert(children && "AddChildElement: the parent element holds no elements");
    children->push_back(child);
}

static void LoadElement(UILoadState& state, const UIMarkupNode& markup, UIElementIndex parent)
{
    if (!parent) {
        if (markup.tag != "ui") { state.Error(markup.line, "root element must be <ui>, not <" + markup.tag + ">"); return; }
    }
    else if (markup.tag == "for") { LoadLoop(state, markup, parent); return; }
    else if (markup.tag == "var") {
        if (!std::holds_alternative<UIRootElement>(state.elements[parent.value].kind))
            state.Error(markup.line, "<var> belongs directly in <ui>");
        return;
    }
    else if (markup.tag == "br") { state.Error(markup.line, "<br/> belongs in <text> or <input>"); return; }
    else if (markup.tag != "box" && markup.tag != "text" && markup.tag != "input" && markup.tag != "scrollbar") {
        state.Error(markup.line, "unknown tag <" + markup.tag + ">");
        return;
    }
    const bool text_leaf = markup.tag == "text" || markup.tag == "input";
    const bool scrollbar = markup.tag == "scrollbar";

    for (const UIMarkupAttribute& attribute : markup.attributes) {
        const bool scrollbar_attribute = scrollbar && (attribute.name == "for" || attribute.name == "axis");
        if (attribute.name != "id" && attribute.name != "class" && !scrollbar_attribute)
            state.Error(attribute.line, "attribute '" + attribute.name + "' is not supported on <" + markup.tag + ">");
        else if (attribute.value.find("{{") != std::string::npos)
            state.Error(attribute.line, "attribute '" + attribute.name + "' takes no {{ }}");
    }
    UIElementStyle style;
    if (const UIMarkupAttribute* id = markup.Attribute("id")) style.id = id->value;
    if (const UIMarkupAttribute* classes = markup.Attribute("class")) style.classes = SplitClasses(classes->value);
    style.style_value    = markup.style_value;
    style.attributes_end = markup.attributes_end;

    UIDeclaredValues values;
    bool material_bound = false;
    LoadDeclarations(state, markup, style, values, material_bound);
    style.bound_properties = BoundPropertiesOf(style.declarations);

    std::vector<UITemplatePart> text;
    if (text_leaf) {
        for (const UIMarkupNode& child : markup.children)
            if (!child.IsText() && child.tag != "br")
                state.Error(child.line, "<" + markup.tag + "> holds only text and <br/>, not <" + child.tag + ">");
        ParseTemplate(state, UIMarkupText(markup), markup.line, text);
        if (markup.tag == "input") {
            if (std::ranges::count_if(text, IsVarReference) != 1)
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
    else if ((markup.tag == "box" || scrollbar) && (!values.material.empty() || material_bound) && values.quad.empty())
        state.Error(markup.line, "<" + markup.tag + "> with material needs quad");

    std::string target;
    UIAxis      axis = UIAxis::Y;
    if (scrollbar) {
        if (!markup.children.empty()) state.Error(markup.line, "<scrollbar> holds nothing");
        if (const UIMarkupAttribute* target_attribute = markup.Attribute("for")) target = target_attribute->value;
        if (target.empty()) state.Error(markup.line, "<scrollbar> needs for: the id of the box it scrolls");
        const UIMarkupAttribute* axis_attribute = markup.Attribute("axis");
        if (axis_attribute && !ParseKeyword(axis_attribute->value, kAxisNames, axis))
            state.Error(axis_attribute->line, "axis takes x or y, not '" + axis_attribute->value + "'");
        if (values.thumb_materials.idle.empty()) state.Error(markup.line, "<scrollbar> needs thumb-material");
        if (values.thumb_quad.empty())           state.Error(markup.line, "<scrollbar> needs thumb-quad");
    }

    UIElement element{ markup.line };
    if (!parent)                    element.kind = UIRootElement{ std::move(style) };
    else if (markup.tag == "box")   element.kind = UIBoxElement{ std::move(style) };
    else if (scrollbar)             element.kind = UIScrollbarElement{ std::move(style), std::move(target), 0, axis };
    else if (markup.tag == "text")  element.kind = UITextElement{ std::move(style), std::move(text) };
    else                            element.kind = UIInputElement{ std::move(style), std::move(text) };

    const UIElementIndex index{ safe_u32(state.elements.size()) };
    if (parent) AddChildElement(state, parent, index);
    state.elements.push_back(std::move(element));
    if (text_leaf || scrollbar) return;
    for (const UIMarkupNode& child : markup.children) {
        if (child.IsText()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, index);
    }
}

static void LoadLoop(UILoadState& state, const UIMarkupNode& markup, UIElementIndex parent)
{
    UILoopElement loop;
    std::string index_name;
    if (ParseLoopHeader(state, markup, index_name, loop.count)) {
        const std::string count = RenderTemplate(loop.count, [&state](const UITemplatePart& part) { return state.ValueOf(part); });
        uint32_t number = 0;
        if (!ParseUnsigned(count, number))
            state.Error(markup.line, "invalid loop count '" + count + "' from '" + markup.header + "'");
    }

    const UIElementIndex index{ safe_u32(state.elements.size()) };
    AddChildElement(state, parent, index);
    state.elements.push_back(UIElement{ markup.line, std::move(loop) });

    state.loop_indices.push_back(index_name == "_" ? std::string() : index_name);
    for (const UIMarkupNode& child : markup.children) {
        if (child.IsText()) { state.Error(child.line, "text belongs in <text>"); continue; }
        LoadElement(state, child, index);
    }
    state.loop_indices.pop_back();
}

struct UIElementPlace {
    UIElementIndex              element;
    std::vector<UIElementIndex> ancestors;
};

static void CollectPlaces(const std::vector<UIElement>& elements, UIElementIndex index, std::vector<UIElementIndex>& ancestors,
                          std::unordered_map<std::string, UIElementPlace>& places_by_id, std::vector<UIElementPlace>& scrollbars)
{
    const UIElement& element = elements[index.value];
    if (const UIElementStyle* style = element.Style(); style && !style->id.empty())
        places_by_id.emplace(style->id, UIElementPlace{ index, ancestors });
    if (std::holds_alternative<UIScrollbarElement>(element.kind)) scrollbars.push_back({ index, ancestors });
    const std::vector<UIElementIndex>* children = element.Children();
    if (!children) return;
    ancestors.push_back(index);
    for (UIElementIndex child : *children) CollectPlaces(elements, child, ancestors, places_by_id, scrollbars);
    ancestors.pop_back();
}

static void LinkScrollbars(UILoadState& state)
{
    std::unordered_map<std::string, UIElementPlace> places_by_id;
    std::vector<UIElementPlace> scrollbars;
    std::vector<UIElementIndex> ancestors;
    CollectPlaces(state.elements, UIElementIndex{ 0 }, ancestors, places_by_id, scrollbars);

    const auto value_of = [&state](const UITemplatePart& part) { return state.ValueOf(part); };
    for (const UIElementPlace& place : scrollbars) {
        UIElement& element = state.elements[place.element.value];
        UIScrollbarElement* scrollbar = std::get_if<UIScrollbarElement>(&element.kind);
        assert(scrollbar && "LinkScrollbars: the place is not a scrollbar");
        const std::string& id = scrollbar->target;
        const auto list = places_by_id.find(id);
        if (list == places_by_id.end()) {
            state.Error(element.line, "<scrollbar for=\"" + id + "\">: no element has this id");
            continue;
        }
        const UIElement& list_element = state.elements[list->second.element.value];
        if (!std::holds_alternative<UIBoxElement>(list_element.kind) && !std::holds_alternative<UIRootElement>(list_element.kind)) {
            state.Error(element.line, "<scrollbar for=\"" + id + "\">: only <box> and <ui> scroll");
            continue;
        }
        if (std::ranges::find(place.ancestors, list->second.element) != place.ancestors.end()) {
            state.Error(element.line, "<scrollbar> inside '" + id + "' would scroll away with its content");
            continue;
        }
        UIElementIndex list_loop;
        uint32_t       list_loop_depth = 0;
        for (UIElementIndex ancestor : list->second.ancestors) {
            if (!std::holds_alternative<UILoopElement>(state.elements[ancestor.value].kind)) continue;
            list_loop = ancestor;
            ++list_loop_depth;
        }
        if (list_loop && std::ranges::find(place.ancestors, list_loop) == place.ancestors.end()) {
            state.Error(element.line, "'" + id + "' repeats in a <for> the scrollbar is outside of: which copy it scrolls is unclear");
            continue;
        }
        const UIStyle    style = RenderValues(list_element.Style()->declarations, value_of, state.path).style;
        const bool       vertical = scrollbar->axis == UIAxis::Y;
        const std::string axis_name = vertical ? "y" : "x";
        if (!(vertical ? style.ScrollsY() : style.ScrollsX())) {
            state.Error(element.line, "'" + id + "' does not scroll along " + axis_name + ": overflow-" + axis_name +
                                      " is neither scroll nor auto");
            continue;
        }
        scrollbar->target_loop_depth = list_loop_depth;
    }
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
    LoadElement(state, document->root, {});
    if (state.errors.empty()) LinkScrollbars(state);
    if (!state.errors.empty()) {
        for (const std::string& error : state.errors) SDL_Log("UI_Manager::Load: %s", error.c_str());
        return false;
    }

    for (const UILoadedVar& var : state.vars) variables.Get(var.id)->default_value = var.default_value;

    const UIVarKey no_instance;
    const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
    const UIElementIndex root_element{ 0 };
    const UINodeIndex root_node = Root(RenderValues(state.elements[root_element.value].Style()->declarations, value_of, path).style);
    elements        = std::move(state.elements);
    document_path   = path;
    document_source = source;
    const UIElementStyle& root_style = *elements[root_element.value].Style();
    nodes[root_node.value].id      = root_style.id;
    nodes[root_node.value].classes = root_style.classes;
    nodes[root_node.value].element = root_element;
    if (HasBoundDeclarations(root_style))
        AddBinding({ .target = UIStyleBinding{ root_node }, .element = root_element });
    std::vector<UISlot> slots = BuildItems(root_element, root_node, no_instance, font_manager);
    nodes[root_node.value].slots = std::move(slots);
    ResolveScrollbarTargets();
    return true;
}

std::string UI_Manager::PartValue(const UITemplatePart& part, const UIVarKey& instance) const
{
    if (const std::string* literal = std::get_if<std::string>(&part)) return *literal;
    if (const UIVarReference* reference = std::get_if<UIVarReference>(&part))
        return GetVar(reference->var, ResolveKey(reference->key, instance));
    if (const UILoopIndexReference* loop_index = std::get_if<UILoopIndexReference>(&part))
        return std::to_string(instance[loop_index->depth]);
    assert(false && "UI_Manager::PartValue: template part of unknown kind");
    return {};
}

bool UI_Manager::IsInputNode(UINodeIndex node) const
{
    return node.value < nodes.size() && std::holds_alternative<UIInputData>(nodes[node.value].kind);
}

bool UI_Manager::InputVarOf(UINodeIndex node, UIVarId& var, UIVarKey& key) const
{
    if (!IsInputNode(node)) return false;
    const std::vector<UITemplatePart>* text = elements[nodes[node.value].element.value].Text();
    if (!text) return false;
    for (const UITemplatePart& part : *text) {
        const UIVarReference* reference = std::get_if<UIVarReference>(&part);
        if (!reference) continue;
        var = reference->var;
        key = ResolveKey(reference->key, nodes[node.value].instance);
        return true;
    }
    return false;
}

std::vector<UISlot> UI_Manager::BuildItems(UIElementIndex element, UINodeIndex parent, const UIVarKey& instance,
                                           const FontManager* font_manager)
{
    std::vector<UISlot> slots;
    const std::vector<UIElementIndex>* children = elements[element.value].Children();
    if (!children) return slots;
    for (UIElementIndex child : *children) {
        if (std::holds_alternative<UILoopElement>(elements[child.value].kind)) {
            slots.push_back(BuildLoop(child, parent, instance, font_manager));
            continue;
        }
        const UINodeIndex node = BuildNode(child, parent, instance, font_manager);
        slots.push_back(node);
        std::vector<UISlot> child_slots = BuildItems(child, node, instance, font_manager);
        nodes[node.value].slots = std::move(child_slots);
    }
    return slots;
}

UINodeIndex UI_Manager::BuildNode(UIElementIndex element_index, UINodeIndex parent, const UIVarKey& instance,
                                  const FontManager* font_manager)
{
    const UIElement& element = elements[element_index.value];
    const UIElementStyle* style = element.Style();
    assert(style && "UI_Manager::BuildNode: the element does not become a node");
    const auto value_of = [this, &instance](const UITemplatePart& part) { return PartValue(part, instance); };
    UIDeclaredValues values = RenderStyle(*style, value_of, document_path);
    UINodeIndex node;
    if (const UITextElement* text = std::get_if<UITextElement>(&element.kind)) {
        values.style.padding    = {};
        values.style.overflow_x = UIOverflow::Visible;
        values.style.overflow_y = UIOverflow::Visible;
        node = Text(parent, values.style, RenderTemplate(text->text, value_of), values.material, values.quad,
                    font_manager->FontIdOf(values.font_family), font_manager);
    }
    else if (const UIInputElement* input = std::get_if<UIInputElement>(&element.kind)) {
        values.style.padding    = {};
        values.style.overflow_x = UIOverflow::Visible;
        values.style.overflow_y = UIOverflow::Visible;
        node = Input(parent, values.style, RenderTemplate(input->text, value_of), values.material, values.quad,
                     font_manager->FontIdOf(values.font_family), font_manager, values.caret_material, values.caret_quad);
    }
    else if (std::holds_alternative<UIBoxElement>(element.kind))
        node = Box(parent, values.style, values.material, values.quad);
    else if (const UIScrollbarElement* scrollbar = std::get_if<UIScrollbarElement>(&element.kind))
        node = Scrollbar(parent, values.style, scrollbar->axis, {}, values.material, values.quad, values.thumb_materials,
                         values.thumb_quad);
    else {
        assert(false && "UI_Manager::BuildNode: element of unknown kind");
        return {};
    }
    nodes[node.value].id       = style->id;
    nodes[node.value].classes  = style->classes;
    nodes[node.value].element  = element_index;
    nodes[node.value].instance = instance;

    if (const std::vector<UITemplatePart>* text = element.Text(); text && HasVars(*text))
        AddBinding({ .target = UITextBinding{ node }, .element = element_index, .instance = instance });
    if (HasBoundDeclarations(*style))
        AddBinding({ .target = UIStyleBinding{ node }, .element = element_index, .instance = instance });
    return node;
}

UILoopIndex UI_Manager::BuildLoop(UIElementIndex element, UINodeIndex parent, const UIVarKey& outer, const FontManager* font_manager)
{
    const UILoopIndex loop{ safe_u32(loops.size()) };
    loops.push_back(UILoop{ element, parent, outer });
    const UILoopElement* loop_element = std::get_if<UILoopElement>(&elements[element.value].kind);
    assert(loop_element && "UI_Manager::BuildLoop: the element is not a loop");
    if (HasVars(loop_element->count))
        AddBinding({ .target = UICountBinding{ loop }, .element = element, .instance = outer });
    uint32_t count = 0;
    if (ReadLoopCount(loop, count)) loops[loop.value].count = count;
    BuildInstances(loop, loops[loop.value].count, font_manager);
    return loop;
}

void UI_Manager::BuildInstances(UILoopIndex loop, uint32_t count, const FontManager* font_manager)
{
    while (loops[loop.value].instances.size() < count) {
        UIVarKey instance = loops[loop.value].outer;
        instance.push_back(safe_u32(loops[loop.value].instances.size()));
        std::vector<UISlot> slots = BuildItems(loops[loop.value].element, loops[loop.value].parent, instance, font_manager);
        loops[loop.value].instances.push_back({ std::move(slots) });
    }
}

bool UI_Manager::ReadLoopCount(UILoopIndex loop, uint32_t& count) const
{
    const UIElement& element = elements[loops[loop.value].element.value];
    const UILoopElement* loop_element = std::get_if<UILoopElement>(&element.kind);
    if (!loop_element) return false;
    const UIVarKey& outer = loops[loop.value].outer;
    const std::string value = RenderTemplate(loop_element->count, [this, &outer](const UITemplatePart& part) { return PartValue(part, outer); });
    if (ParseUnsigned(value, count)) return true;
    SDL_Log("UI_Manager: %s:%u: invalid loop count '%s'", document_path.c_str(), element.line, value.c_str());
    return false;
}

void UI_Manager::ResizeLoop(UILoopIndex loop, uint32_t count, const FontManager* font_manager)
{
    loops[loop.value].count = count;
    if (count > loops[loop.value].instances.size()) {
        BuildInstances(loop, count, font_manager);
        RebuildChildren(loops[loop.value].parent);
        ResolveScrollbarTargets();
    }
    UpdateParking(loop);
}

void UI_Manager::UpdateParking(UILoopIndex loop)
{
    for (uint32_t index = 0; index < loops[loop.value].instances.size(); ++index) {
        const bool parked = loops[loop.value].parked || index >= loops[loop.value].count;
        for (const UISlot& slot : loops[loop.value].instances[index].slots) {
            if (const UINodeIndex* node = std::get_if<UINodeIndex>(&slot)) {
                SetNodeParked(*node, parked);
            }
            else if (const UILoopIndex* inner_loop = std::get_if<UILoopIndex>(&slot)) {
                loops[inner_loop->value].parked = parked;
                UpdateParking(*inner_loop);
            }
            else
                assert(false && "UI_Manager::UpdateParking: slot of unknown kind");
        }
    }
}

void UI_Manager::AppendSlotNodes(const std::vector<UISlot>& slots, std::vector<UINodeIndex>& out) const
{
    for (const UISlot& slot : slots) {
        if (const UINodeIndex* node = std::get_if<UINodeIndex>(&slot))
            out.push_back(*node);
        else if (const UILoopIndex* loop = std::get_if<UILoopIndex>(&slot))
            for (const UILoopInstance& instance : loops[loop->value].instances) AppendSlotNodes(instance.slots, out);
        else
            assert(false && "UI_Manager::AppendSlotNodes: slot of unknown kind");
    }
}

void UI_Manager::AddBinding(UIBinding binding)
{
    const UIBindingIndex index{ safe_u32(bindings.size()) };
    const auto link = [this, index, &binding](const std::vector<UITemplatePart>& parts) {
        for (const UITemplatePart& part : parts) {
            const UIVarReference* reference = std::get_if<UIVarReference>(&part);
            UIVar* var = reference ? variables.Get(reference->var) : nullptr;
            if (!var) continue;
            std::vector<UIBindingIndex>& linked = var->elements[ResolveKey(reference->key, binding.instance)].bindings;
            if (linked.empty() || linked.back() != index) linked.push_back(index);
        }
    };
    const UIElement&                   element = elements[binding.element.value];
    const std::vector<UITemplatePart>* text    = element.Text();
    const UIElementStyle*              style   = element.Style();
    const UILoopElement*               loop    = std::get_if<UILoopElement>(&element.kind);
    if (std::holds_alternative<UITextBinding>(binding.target) && text)
        link(*text);
    else if (std::holds_alternative<UIStyleBinding>(binding.target) && style)
        for (const UIDeclaration& declaration : style->declarations) link(declaration.value);
    else if (std::holds_alternative<UICountBinding>(binding.target) && loop)
        link(loop->count);
    else
        assert(false && "UI_Manager::AddBinding: the binding does not match its element");
    bindings.push_back(std::move(binding));
}

void UI_Manager::ResolveScrollbarTargets()
{
    for (UINode& entry : nodes) {
        UIScrollbarData* scrollbar = std::get_if<UIScrollbarData>(&entry.kind);
        if (!scrollbar || scrollbar->target || entry.element.value >= elements.size()) continue;
        const UIScrollbarElement* element = std::get_if<UIScrollbarElement>(&elements[entry.element.value].kind);
        if (!element) continue;
        assert(element->target_loop_depth <= entry.instance.size() && "ResolveScrollbarTargets: the list sits in loops the scrollbar is outside of");
        const UIVarKey list_instance(entry.instance.begin(), entry.instance.begin() + element->target_loop_depth);
        scrollbar->target = FindNode(element->target, list_instance);
    }
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
    for (UIBindingIndex index : element.bindings) {
        UIBinding& binding = bindings[index.value];
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
        bindings[dirty_bindings[i].value].queued = false;
        // Копия: постройка экземпляров добавляет связи, и вектор связей переезжает.
        const UIBinding  binding = bindings[dirty_bindings[i].value];
        const UIElement& element = elements[binding.element.value];
        const auto value_of = [this, &binding](const UITemplatePart& part) { return PartValue(part, binding.instance); };
        const std::vector<UITemplatePart>* text  = element.Text();
        const UIElementStyle*              style = element.Style();
        if (const UITextBinding* text_binding = std::get_if<UITextBinding>(&binding.target); text_binding && text) {
            SetText(text_binding->node, RenderTemplate(*text, value_of), font_manager);
        }
        else if (const UIStyleBinding* style_binding = std::get_if<UIStyleBinding>(&binding.target); style_binding && style) {
            // Стиль собирается заново из всех объявлений узла: связанное объявление может перекрываться
            // следующими, в том числе частично (padding и padding-left).
            const UIDeclaredValues values = RenderStyle(*style, value_of, document_path);
            SetNodeStyle(style_binding->node, values.style);
            SetNodeMaterial(ctx, style_binding->node, values.material);
        }
        else if (const UICountBinding* count_binding = std::get_if<UICountBinding>(&binding.target)) {
            uint32_t count = 0;
            if (!ReadLoopCount(count_binding->loop, count)) continue;
            if (count > loops[count_binding->loop.value].instances.size() && !structure_lock.owns_lock())
                structure_lock.lock();
            ResizeLoop(count_binding->loop, count, font_manager);
        }
        else
            assert(false && "UI_Manager::ApplyDirtyBindings: the binding does not match its element");
    }
    dirty_bindings.clear();
}

// У экземпляров цикла один элемент на всех: правку одного экземпляра файл не выражает.
UIElementStyle* UI_Manager::EditableStyle(UINodeIndex node)
{
    const bool from_file = node.value < nodes.size() && nodes[node.value].element.value < elements.size();
    if (!from_file || !nodes[node.value].instance.empty()) return nullptr;
    return elements[nodes[node.value].element.value].Style();
}

void UI_Manager::SetEdit(UIElementStyle& style, UIProperty property, std::string value, const std::string& file_value)
{
    const auto edit = std::ranges::find(style.edits, property, &UIEdit::property);
    if (value == file_value) {
        if (edit == style.edits.end()) return;
        style.edits.erase(edit);
        --unsaved_edits;
        return;
    }
    if (edit != style.edits.end()) { edit->value = std::move(value); return; }
    style.edits.push_back({ property, std::move(value) });
    ++unsaved_edits;
}

void UI_Manager::EditNodeStyle(UINodeIndex node, const UIStyle& style)
{
    if (node.value >= nodes.size()) return;
    if (UIElementStyle* element_style = EditableStyle(node)) {
        const UIVarKey no_instance;
        const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
        const UIStyle file_style = RenderValues(element_style->declarations, value_of, document_path).style;
        for (uint8_t index = 0; index < std::to_underlying(UIProperty::Count); ++index) {
            const UIProperty property{ index };
            if (property == UIProperty::Material || (element_style->bound_properties & UIPropertyBit(property))) continue;
            std::string value = StyleValue(style, property);
            if (value != StyleValue(nodes[node.value].style, property))
                SetEdit(*element_style, property, std::move(value), StyleValue(file_style, property));
        }
    }
    SetNodeStyle(node, style);
}

void UI_Manager::EditNodeMaterial(EngineContext* ctx, UINodeIndex node, const std::string& material)
{
    if (node.value >= nodes.size() || (nodes[node.value].TextData() && material.empty())) return;
    UIElementStyle* element_style = EditableStyle(node);
    if (element_style && !(element_style->bound_properties & UIPropertyBit(UIProperty::Material))) {
        const UIVarKey no_instance;
        const auto value_of = [this, &no_instance](const UITemplatePart& part) { return PartValue(part, no_instance); };
        std::string value = MaterialValue(material);
        if (IsWritableValue(value))
            SetEdit(*element_style, UIProperty::Material, std::move(value),
                    MaterialValue(RenderValues(element_style->declarations, value_of, document_path).material));
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
        const UIElementStyle* element_style = element.Style();
        if (!element_style) continue;
        std::string appended;
        for (const UIEdit& edit : element_style->edits) {
            const std::string_view property = PropertyName(edit.property);
            const UIDeclaration* winner = WinningDeclaration(element_style->declarations, edit.property);
            if (winner && winner->from_style_attribute && winner->property == property && !HasSubstitutions(winner->value)) {
                patches.push_back({ winner->span.offset, winner->span.length, edit.value });
                continue;
            }
            if (!appended.empty()) appended += "; ";
            appended.append(property).append(": ").append(edit.value);
        }
        if (appended.empty()) continue;

        if (!element_style->style_value) {
            patches.push_back({ element_style->attributes_end, 0, " style=\"" + appended + "\"" });
            continue;
        }
        const UIMarkupSpan span = *element_style->style_value;
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
