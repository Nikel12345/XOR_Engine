#include "PCH.h"
#include "UI_Markup.h"
#include <bit>
#include <charconv>

static bool IsSpace(char c)     { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool IsNameChar(char c)  { return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-'; }

static bool IsName(std::string_view s)
{
    if (s.empty()) return false;
    for (char c : s) if (!IsNameChar(c)) return false;
    return true;
}

static std::string_view Trim(std::string_view s)
{
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back()))  s.remove_suffix(1);
    return s;
}

static void AppendByte(std::string& out, uint32_t byte) { out += std::bit_cast<char>(safe_u32_u8(byte)); }

static void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        AppendByte(out, cp);
    } else if (cp < 0x800) {
        AppendByte(out, 0xC0 | (cp >> 6));
        AppendByte(out, 0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        AppendByte(out, 0xE0 | (cp >> 12));
        AppendByte(out, 0x80 | ((cp >> 6) & 0x3F));
        AppendByte(out, 0x80 | (cp & 0x3F));
    } else {
        AppendByte(out, 0xF0 | (cp >> 18));
        AppendByte(out, 0x80 | ((cp >> 12) & 0x3F));
        AppendByte(out, 0x80 | ((cp >> 6) & 0x3F));
        AppendByte(out, 0x80 | (cp & 0x3F));
    }
}

// Как HTML: '&', за которым нет известной сущности, остаётся буквой.
static std::string DecodeEntities(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        const size_t semi = raw[i] == '&' ? raw.find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos || semi - i > 10) { out += raw[i++]; continue; }

        const std::string_view name = raw.substr(i + 1, semi - i - 1);
        if      (name == "lt")   out += '<';
        else if (name == "gt")   out += '>';
        else if (name == "amp")  out += '&';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            uint32_t cp = 0;
            const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size() || cp == 0 || cp > 0x10FFFF) {
                out += raw[i++];
                continue;
            }
            AppendUtf8(out, cp);
        }
        else { out += raw[i++]; continue; }
        i = semi + 1;
    }
    return out;
}

// base — смещение куска от начала файла: от него считаются UIMarkupSpan.
struct MarkupReader {
    std::string_view src;
    size_t           base = 0;
    uint32_t         line = 1;
    size_t           pos  = 0;
    UIMarkupError    error;

    bool AtEnd() const { return pos >= src.size(); }
    char Peek(size_t ahead = 0) const { return pos + ahead < src.size() ? src[pos + ahead] : '\0'; }
    bool StartsWith(std::string_view s) const { return src.substr(pos).starts_with(s); }

    void Advance(size_t n = 1)
    {
        for (; n > 0 && pos < src.size(); --n) {
            if (src[pos] == '\n') ++line;
            ++pos;
        }
    }
    void SkipSpaces() { while (!AtEnd() && IsSpace(Peek())) Advance(); }

    std::string ReadName()
    {
        const size_t begin = pos;
        while (!AtEnd() && IsNameChar(Peek())) Advance();
        return std::string(src.substr(begin, pos - begin));
    }

    bool SkipPast(std::string_view terminator)
    {
        const size_t found = src.find(terminator, pos);
        if (found == std::string_view::npos) return false;
        Advance(found + terminator.size() - pos);
        return true;
    }

    UIMarkupSpan Span(size_t begin, size_t length) const { return { safe_u32(base + begin), safe_u32(length) }; }

    bool Fail(uint32_t at_line, std::string message)
    {
        error = { at_line, std::move(message) };
        return false;
    }
};

static bool SkipCssSpace(MarkupReader& r)
{
    while (true) {
        r.SkipSpaces();
        if (!r.StartsWith("/*")) return true;
        const uint32_t comment_line = r.line;
        if (!r.SkipPast("*/")) return r.Fail(comment_line, "unclosed /* comment");
    }
}

static bool ParseSelectors(std::string_view text, uint32_t line, std::vector<UIMarkupSelector>& out, UIMarkupError& error)
{
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = std::min(text.find(',', begin), text.size());
        const std::string_view part = Trim(text.substr(begin, comma - begin));
        begin = comma + 1;

        UIMarkupSelector selector;
        if (part == "*") {
            selector.kind = UIMarkupSelectorKind::Universal;
        } else if (part.size() > 1 && (part[0] == '.' || part[0] == '#') && IsName(part.substr(1))) {
            selector.kind = part[0] == '.' ? UIMarkupSelectorKind::Class : UIMarkupSelectorKind::Id;
            selector.name = part.substr(1);
        } else if (!part.empty() && IsNameStart(part[0]) && IsName(part)) {
            selector.kind = UIMarkupSelectorKind::Tag;
            selector.name = part;
        } else {
            error = { line, "selector '" + std::string(part) + "' is not supported: only *, tag, .class, #id" };
            return false;
        }
        out.push_back(std::move(selector));
    }
    return true;
}

static bool ParseDeclarations(MarkupReader& r, std::vector<UIMarkupDeclaration>& out)
{
    while (true) {
        if (!SkipCssSpace(r)) return false;
        if (r.AtEnd()) return true;
        if (r.Peek() == ';') { r.Advance(); continue; }

        const uint32_t decl_line = r.line;
        if (!IsNameStart(r.Peek()) && r.Peek() != '-') return r.Fail(decl_line, "expected a property name");
        std::string property = r.ReadName();
        if (!SkipCssSpace(r)) return false;
        if (r.Peek() != ':') return r.Fail(decl_line, "expected ':' after '" + property + "'");
        r.Advance();
        r.SkipSpaces();

        const size_t value_begin = r.pos;
        while (!r.AtEnd() && r.Peek() != ';') r.Advance();
        const std::string_view value = Trim(r.src.substr(value_begin, r.pos - value_begin));
        if (value.empty()) return r.Fail(decl_line, "property '" + property + "' has no value");
        out.push_back({ std::move(property), std::string(value), decl_line, r.Span(value_begin, value.size()) });
    }
}

// npos — тело не закрыто или в нём вложенный блок.
static size_t FindBlockEnd(std::string_view s, size_t from, bool& nested)
{
    nested = false;
    for (size_t i = from; i < s.size(); ++i) {
        if (s.substr(i).starts_with("/*")) {
            const size_t close = s.find("*/", i + 2);
            if (close == std::string_view::npos) return std::string_view::npos;
            i = close + 1;
        }
        else if (s.substr(i).starts_with("{{")) {
            const size_t close = s.find("}}", i + 2);
            if (close == std::string_view::npos) return std::string_view::npos;
            i = close + 1;
        }
        else if (s[i] == '{') { nested = true; return std::string_view::npos; }
        else if (s[i] == '}') return i;
    }
    return std::string_view::npos;
}

static bool ParseRules(MarkupReader& r, std::vector<UIMarkupRule>& rules)
{
    while (true) {
        if (!SkipCssSpace(r)) return false;
        if (r.AtEnd()) return true;
        if (r.Peek() == '@') return r.Fail(r.line, "@-rules are not supported");

        UIMarkupRule rule;
        rule.line = r.line;
        const size_t selectors_begin = r.pos;
        while (!r.AtEnd() && r.Peek() != '{' && r.Peek() != '}') r.Advance();
        if (r.Peek() != '{') return r.Fail(rule.line, "expected '{' after a selector");
        if (!ParseSelectors(r.src.substr(selectors_begin, r.pos - selectors_begin), rule.line, rule.selectors, r.error))
            return false;
        r.Advance();

        bool nested = false;
        const size_t body_end = FindBlockEnd(r.src, r.pos, nested);
        if (body_end == std::string_view::npos)
            return r.Fail(rule.line, nested ? "nested blocks are not supported" : "rule is not closed with '}'");

        MarkupReader body{ r.src.substr(r.pos, body_end - r.pos), r.base + r.pos, r.line };
        if (!ParseDeclarations(body, rule.declarations)) { r.error = body.error; return false; }
        r.Advance(body_end + 1 - r.pos);
        rules.push_back(std::move(rule));
    }
}

static bool IsStyleOpen(const MarkupReader& r) { return r.StartsWith("<style") && !IsNameChar(r.Peek(6)); }

static bool ParseStyleBlock(MarkupReader& r, UIMarkupDocument& doc)
{
    const uint32_t open_line = r.line;
    r.Advance(6);
    r.SkipSpaces();
    if (r.StartsWith("/>")) { r.Advance(2); return true; }
    if (r.Peek() != '>') return r.Fail(open_line, "<style> takes no attributes");
    r.Advance();

    const size_t end = r.src.find("</style", r.pos);
    if (end == std::string_view::npos) return r.Fail(open_line, "<style> is not closed");

    MarkupReader css{ r.src.substr(r.pos, end - r.pos), r.base + r.pos, r.line };
    if (!ParseRules(css, doc.rules)) { r.error = css.error; return false; }

    r.Advance(end - r.pos + 7);
    r.SkipSpaces();
    if (r.Peek() != '>') return r.Fail(r.line, "malformed </style>");
    r.Advance();
    return true;
}

static bool ParseElement(MarkupReader& r, UIMarkupDocument& doc, UIMarkupNode& node);

static bool ParseContent(MarkupReader& r, UIMarkupDocument& doc, UIMarkupNode& node)
{
    std::string text;
    uint32_t    text_line = 0;
    const auto flush_text = [&] {
        if (text_line != 0) node.children.push_back(UIMarkupNode{ .text = std::move(text), .line = text_line });
        text.clear();
        text_line = 0;
    };

    while (true) {
        if (r.AtEnd()) return r.Fail(node.line, "<" + node.tag + "> is not closed");

        if (r.StartsWith("<!--")) {
            const uint32_t comment_line = r.line;
            if (!r.SkipPast("-->")) return r.Fail(comment_line, "unclosed <!-- comment");
        }
        else if (r.StartsWith("</")) {
            flush_text();
            const uint32_t close_line = r.line;
            r.Advance(2);
            const std::string name = r.ReadName();
            r.SkipSpaces();
            if (r.Peek() != '>') return r.Fail(close_line, "malformed closing tag </" + name);
            r.Advance();
            if (name != node.tag)
                return r.Fail(close_line, "</" + name + "> closes <" + node.tag + "> opened at line " + std::to_string(node.line));
            return true;
        }
        else if (IsStyleOpen(r)) {
            flush_text();
            if (!ParseStyleBlock(r, doc)) return false;
        }
        else if (r.Peek() == '<') {
            flush_text();
            if (!IsNameStart(r.Peek(1))) return r.Fail(r.line, "'<' in text must be written as &lt;");
            node.children.emplace_back();
            if (!ParseElement(r, doc, node.children.back())) return false;
        }
        else {
            const size_t run_begin = r.pos;
            while (!r.AtEnd() && r.Peek() != '<') {
                if (text_line == 0 && !IsSpace(r.Peek())) text_line = r.line;
                r.Advance();
            }
            text += DecodeEntities(r.src.substr(run_begin, r.pos - run_begin));
        }
    }
}

static bool ParseElement(MarkupReader& r, UIMarkupDocument& doc, UIMarkupNode& node)
{
    node.line = r.line;
    r.Advance();
    if (!IsNameStart(r.Peek())) return r.Fail(node.line, "expected a tag name after '<'");
    node.tag = r.ReadName();

    // Заголовок <for> — выражение, а не атрибуты: его разбирает загрузчик.
    if (node.tag == "for") {
        const size_t header_begin = r.pos;
        while (!r.AtEnd() && r.Peek() != '>') r.Advance();
        if (r.AtEnd()) return r.Fail(node.line, "<for is not closed with '>'");
        const std::string_view header = Trim(r.src.substr(header_begin, r.pos - header_begin));
        r.Advance();
        if (header.ends_with('/')) return r.Fail(node.line, "<for> needs a body closed with </for>");
        node.header = header;
        return ParseContent(r, doc, node);
    }

    bool has_style = false;
    while (true) {
        r.SkipSpaces();
        if (r.StartsWith("/>")) { r.Advance(2); return true; }
        if (r.Peek() == '>') { r.Advance(); break; }
        if (r.AtEnd()) return r.Fail(node.line, "<" + node.tag + " is not closed with '>'");
        if (!IsNameStart(r.Peek())) return r.Fail(r.line, "unexpected '" + std::string(1, r.Peek()) + "' in <" + node.tag + ">");

        const uint32_t attr_line = r.line;
        std::string name = r.ReadName();
        r.SkipSpaces();
        if (r.Peek() != '=') return r.Fail(attr_line, "attribute '" + name + "' needs a value");
        r.Advance();
        r.SkipSpaces();
        const char quote = r.Peek();
        if (quote != '"' && quote != '\'') return r.Fail(attr_line, "value of '" + name + "' must be quoted");
        r.Advance();

        const size_t   value_begin = r.pos;
        const uint32_t value_line  = r.line;
        while (!r.AtEnd() && r.Peek() != quote) r.Advance();
        if (r.AtEnd()) return r.Fail(attr_line, "value of '" + name + "' is not closed");
        const std::string_view raw = r.src.substr(value_begin, r.pos - value_begin);
        r.Advance();

        if (node.Attribute(name) || (name == "style" && has_style))
            return r.Fail(attr_line, "duplicate attribute '" + name + "'");
        if (name == "style") {
            MarkupReader css{ raw, r.base + value_begin, value_line };
            if (!ParseDeclarations(css, node.style)) { r.error = css.error; return false; }
            has_style = true;
        } else {
            node.attributes.push_back({ std::move(name), DecodeEntities(raw), attr_line, r.Span(value_begin, raw.size()) });
        }
    }

    if (node.tag == "br") return true;
    return ParseContent(r, doc, node);
}

static bool CheckUniqueIds(const UIMarkupNode& node, std::unordered_map<std::string, uint32_t>& seen, UIMarkupError& error)
{
    if (const UIMarkupAttribute* id = node.Attribute("id")) {
        const auto [it, inserted] = seen.emplace(id->value, id->line);
        if (!inserted) {
            error = { id->line, "duplicate id '" + id->value + "' (first at line " + std::to_string(it->second) + ")" };
            return false;
        }
    }
    for (const UIMarkupNode& child : node.children)
        if (!CheckUniqueIds(child, seen, error)) return false;
    return true;
}

const UIMarkupAttribute* UIMarkupNode::Attribute(std::string_view name) const
{
    for (const UIMarkupAttribute& a : attributes) if (a.name == name) return &a;
    return nullptr;
}

std::expected<UIMarkupDocument, UIMarkupError> ParseUIMarkup(std::string_view source)
{
    MarkupReader r{ source };
    if (r.StartsWith("\xEF\xBB\xBF")) r.pos = 3;

    UIMarkupDocument doc;
    bool has_root = false;
    while (true) {
        r.SkipSpaces();
        if (r.AtEnd()) break;

        if (r.StartsWith("<!--")) {
            const uint32_t comment_line = r.line;
            if (!r.SkipPast("-->")) return std::unexpected(UIMarkupError{ comment_line, "unclosed <!-- comment" });
        }
        else if (r.StartsWith("<!")) {
            const uint32_t decl_line = r.line;
            if (!r.SkipPast(">")) return std::unexpected(UIMarkupError{ decl_line, "unclosed <! declaration" });
        }
        else if (IsStyleOpen(r)) {
            if (!ParseStyleBlock(r, doc)) return std::unexpected(r.error);
        }
        else if (r.Peek() != '<') {
            return std::unexpected(UIMarkupError{ r.line, "text outside the root element" });
        }
        else if (has_root) {
            return std::unexpected(UIMarkupError{ r.line, "second root element: the file holds one" });
        }
        else {
            if (!ParseElement(r, doc, doc.root)) return std::unexpected(r.error);
            has_root = true;
        }
    }
    if (!has_root) return std::unexpected(UIMarkupError{ r.line, "no root element" });

    std::unordered_map<std::string, uint32_t> ids;
    UIMarkupError error;
    if (!CheckUniqueIds(doc.root, ids, error)) return std::unexpected(std::move(error));
    return doc;
}

std::vector<const UIMarkupDeclaration*> ResolveUIStyle(const UIMarkupDocument& doc, const UIMarkupNode& element)
{
    const UIMarkupAttribute* id      = element.Attribute("id");
    const UIMarkupAttribute* classes = element.Attribute("class");
    const auto has_class = [classes](std::string_view name) {
        if (!classes) return false;
        std::string_view rest = classes->value;
        while (!rest.empty()) {
            const size_t begin = std::min(rest.find_first_not_of(" \t\n\r\f"), rest.size());
            rest.remove_prefix(begin);
            const size_t end = std::min(rest.find_first_of(" \t\n\r\f"), rest.size());
            if (end > 0 && rest.substr(0, end) == name) return true;
            rest.remove_prefix(end);
        }
        return false;
    };

    struct Match { UIMarkupSelectorKind specificity; size_t rule; };
    std::vector<Match> matches;
    for (size_t i = 0; i < doc.rules.size(); ++i) {
        bool matched = false;
        UIMarkupSelectorKind best = UIMarkupSelectorKind::Universal;
        for (const UIMarkupSelector& s : doc.rules[i].selectors) {
            bool hit = false;
            switch (s.kind) {
                case UIMarkupSelectorKind::Universal: hit = true;                            break;
                case UIMarkupSelectorKind::Tag:       hit = s.name == element.tag;           break;
                case UIMarkupSelectorKind::Class:     hit = has_class(s.name);               break;
                case UIMarkupSelectorKind::Id:        hit = id && id->value == s.name;       break;
            }
            // Значения UIMarkupSelectorKind упорядочены по специфичности.
            if (hit && (!matched || s.kind > best)) best = s.kind;
            matched = matched || hit;
        }
        if (matched) matches.push_back({ best, i });
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.specificity < b.specificity; });

    std::vector<const UIMarkupDeclaration*> out;
    for (const Match& m : matches)
        for (const UIMarkupDeclaration& d : doc.rules[m.rule].declarations) out.push_back(&d);
    for (const UIMarkupDeclaration& d : element.style) out.push_back(&d);
    return out;
}

std::string UIMarkupText(const UIMarkupNode& element)
{
    std::string out;
    bool pending_space = false;
    for (const UIMarkupNode& child : element.children) {
        if (child.tag == "br") { out += '\n'; pending_space = false; continue; }
        if (!child.IsText()) continue;
        for (char c : child.text) {
            if (IsSpace(c)) { pending_space = true; continue; }
            if (pending_space && !out.empty() && out.back() != '\n') out += ' ';
            pending_space = false;
            out += c;
        }
    }
    return out;
}
