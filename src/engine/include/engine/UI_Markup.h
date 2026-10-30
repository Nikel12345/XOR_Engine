#pragma once
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// Разметка UI-файла: подмножество HTML со встроенным CSS. О движке не знает — что значат теги и
// свойства, решает тот, кто строит из документа дерево узлов.

// Значение как оно записано в файле: по этому месту правка из редактора перепишет литерал.
struct UIMarkupSpan {
    uint32_t offset = 0;   // байт от начала файла
    uint32_t length = 0;
};

struct UIMarkupAttribute {
    std::string  name;
    std::string  value;    // сущности (&lt; …) раскрыты
    uint32_t     line = 0;
    UIMarkupSpan span;
};

struct UIMarkupDeclaration {
    std::string  property;
    std::string  value;
    uint32_t     line = 0;
    UIMarkupSpan span;
};

enum class UIMarkupSelectorKind : uint8_t { Universal, Tag, Class, Id };   // порядок = специфичность

struct UIMarkupSelector {
    UIMarkupSelectorKind kind = UIMarkupSelectorKind::Universal;
    std::string          name;
};

struct UIMarkupRule {
    std::vector<UIMarkupSelector>    selectors;   // список через запятую: правило подходит по любому
    std::vector<UIMarkupDeclaration> declarations;
    uint32_t                         line = 0;
};

struct UIMarkupNode {
    std::string                      tag;          // пусто — текстовый узел
    std::string                      text;         // у текстового узла: как в файле, сущности раскрыты
    std::vector<UIMarkupAttribute>   attributes;   // кроме style: он разобран в style
    std::vector<UIMarkupDeclaration> style;
    std::vector<UIMarkupNode>        children;
    uint32_t                         line = 0;

    const UIMarkupAttribute* Attribute(std::string_view name) const;
};

struct UIMarkupDocument {
    UIMarkupNode              root;
    std::vector<UIMarkupRule> rules;   // все <style> файла в порядке исходника
};

struct UIMarkupError {
    uint32_t    line = 0;
    std::string message;
};

std::expected<UIMarkupDocument, UIMarkupError> ParseUIMarkup(std::string_view source);

// Объявления элемента в порядке подстановки: *, тег, .class, #id, затем style. Внутри одной
// специфичности — по порядку правил в файле. Свойство, встреченное позже, перекрывает раннее.
std::vector<const UIMarkupDeclaration*> ResolveUIStyle(const UIMarkupDocument& doc, const UIMarkupNode& element);

// Текст элемента по правилам HTML: серии пробелов и переводов строки — один пробел, по краям и у
// переноса пробелы срезаны, <br/> — '\n'.
std::string UIMarkupText(const UIMarkupNode& element);
