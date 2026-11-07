#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct UIMarkupSpan {
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct UIMarkupAttribute {
    std::string  name;
    std::string  value;
    uint32_t     line = 0;
    UIMarkupSpan span;
};

struct UIMarkupDeclaration {
    std::string  property;
    std::string  value;
    uint32_t     line = 0;
    UIMarkupSpan span;
};

enum class UIMarkupSelectorKind : uint8_t { Universal, Tag, Class, Id };

struct UIMarkupSelector {
    UIMarkupSelectorKind kind = UIMarkupSelectorKind::Universal;
    std::string          name;
};

struct UIMarkupRule {
    std::vector<UIMarkupSelector>    selectors;
    std::vector<UIMarkupDeclaration> declarations;
    uint32_t                         line = 0;
};

struct UIMarkupNode {
    std::string                      tag;
    std::string                      text;
    std::string                      header;
    std::vector<UIMarkupAttribute>   attributes;
    std::vector<UIMarkupDeclaration> style;
    std::optional<UIMarkupSpan>      style_value;
    uint32_t                         attributes_end = 0;
    std::vector<UIMarkupNode>        children;
    uint32_t                         line = 0;

    bool IsText() const { return tag.empty(); }
    const UIMarkupAttribute* Attribute(std::string_view name) const;
};

struct UIMarkupDocument {
    UIMarkupNode              root;
    std::vector<UIMarkupRule> rules;
};

struct UIMarkupError {
    uint32_t    line = 0;
    std::string message;
};

std::expected<UIMarkupDocument, UIMarkupError> ParseUIMarkup(std::string_view source);

std::vector<const UIMarkupDeclaration*> ResolveUIStyle(const UIMarkupDocument& doc, const UIMarkupNode& element);

std::string UIMarkupText(const UIMarkupNode& element);
