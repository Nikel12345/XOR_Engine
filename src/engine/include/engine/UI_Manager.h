#pragma once
#include <charconv>
#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Aliases.h"
#include "ComponentStorage.h"
#include "ResourceId.h"
#include "ResourceRegistry.h"
#include "UI_Index.h"
#include "UI_Interaction.h"
#include "UI_Markup.h"

class EngineContext;
class FontManager;
class ObjectManager;
struct SceneData;
struct YGNode;
struct YGConfig;

enum class UIDisplay : uint8_t { Flex, None };
enum class UIDir     : uint8_t { Row, Column };
enum class UIJustify : uint8_t { Start, Center, End, SpaceBetween };
enum class UIAlign   : uint8_t { Start, Center, End, Stretch };
enum class UISize    : uint8_t { Auto, Points, Percent, Grow };
enum class UILimit   : uint8_t { None, Points, Percent };
enum class UIOverflow : uint8_t { Visible, Hidden, Scroll, Auto };

struct UIEdges {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;
    bool operator==(const UIEdges&) const = default;
};

struct UIStyle {
    UIDisplay display     = UIDisplay::Flex;
    UIDir     direction   = UIDir::Column;
    UIJustify justify     = UIJustify::Start;
    UIAlign   align       = UIAlign::Stretch;
    UISize    width_mode  = UISize::Auto;
    float     width       = 0.0f;
    UISize    height_mode = UISize::Auto;
    float     height      = 0.0f;
    UILimit   min_width_mode  = UILimit::None;
    float     min_width       = 0.0f;
    UILimit   max_width_mode  = UILimit::None;
    float     max_width       = 0.0f;
    UILimit   min_height_mode = UILimit::None;
    float     min_height      = 0.0f;
    UILimit   max_height_mode = UILimit::None;
    float     max_height      = 0.0f;
    float     grow        = 0.0f;
    float     shrink      = 0.0f;
    UIEdges   padding;
    UIEdges   margin;
    float     gap         = 0.0f;
    UIOverflow overflow_x = UIOverflow::Visible;
    UIOverflow overflow_y = UIOverflow::Visible;
    UISize    caret_width_mode = UISize::Points;
    float     caret_width      = 2.0f;
    UISize    caret_gap_mode   = UISize::Points;
    float     caret_gap        = 0.0f;

    float FlexGrow() const
    {
        if (grow > 0.0f)                                  return grow;
        if (height_mode == UISize::Grow && height > 0.0f) return height;
        if (width_mode == UISize::Grow)                   return width > 0.0f ? width : 1.0f;
        return 0.0f;
    }

    bool Clips() const { return overflow_x != UIOverflow::Visible || overflow_y != UIOverflow::Visible; }
    bool ScrollsX() const { return overflow_x == UIOverflow::Scroll || overflow_x == UIOverflow::Auto; }
    bool ScrollsY() const { return overflow_y == UIOverflow::Scroll || overflow_y == UIOverflow::Auto; }
    bool Scrolls() const { return ScrollsX() || ScrollsY(); }
};

enum class UIProperty : uint8_t {
    Display, FlexDirection, JustifyContent, AlignItems, Width, Height,
    MinWidth, MaxWidth, MinHeight, MaxHeight, FlexGrow, FlexShrink, Gap,
    PaddingTop, PaddingRight, PaddingBottom, PaddingLeft,
    MarginTop, MarginRight, MarginBottom, MarginLeft,
    OverflowX, OverflowY,
    Material,
    Count
};

constexpr uint32_t UIPropertyBit(UIProperty property) { return 1u << std::to_underlying(property); }

struct UITextShape {
    struct Glyph {
        uint32_t code;
        float    advance;
        bool     space;
        bool operator==(const Glyph&) const = default;
    };
    std::vector<Glyph> glyphs;
    float              line_height = 0.0f;
};

struct UIGlyphPlace {
    uint32_t wrapped_index = 0;
    int      line = 0;
    float    x = 0.0f;
};

using UISlot = std::variant<UINodeIndex, UILoopIndex>;

struct UIClipArea {
    uint32_t index = 0;
    float    left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

struct UIContentBox {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;

    bool Contains(float x, float y) const { return x >= left && x <= right && y >= top && y <= bottom; }
};

struct UIScrollView {
    UINodeIndex node;
    float       scroll_x = 0.0f, scroll_y = 0.0f;
    float       range_x = 0.0f, range_y = 0.0f;
};

struct UIHit {
    float       z = std::numeric_limits<float>::max();
    UINodeIndex node;
    UINodeIndex clickable;
};

struct UIBoxData {};

struct UITextData {
    std::unique_ptr<UITextShape> shape;
    FontId                       font;
    std::vector<uint32_t>        wrapped_glyphs;
    std::vector<UIGlyphPlace>    glyph_places;
};

struct UIInputData {
    UITextData  text;
    std::string caret_material;
    std::string caret_quad;
    Entity      caret_entity = 0;
};

using UINodeKind = std::variant<UIBoxData, UITextData, UIInputData>;

struct UINode {
    YGNode*                  yoga_node = nullptr;
    UIStyle                  style;
    UINodeKind               kind;
    bool                     draws = false;
    std::string              material;
    std::string              quad;
    std::string              id;
    std::vector<std::string> classes;
    UIElementIndex           element;
    UIVarKey                 instance;
    UINodeIndex              parent;
    std::vector<UINodeIndex> children;
    std::vector<UISlot>      slots;
    float                    offset_x = 0.0f, offset_y = 0.0f, offset_z = 0.0f;
    glm::mat4                local{ 1.0f };
    float                    ndc_center_x = 0.0f, ndc_center_y = 0.0f, ndc_z = 0.5f;
    float                    layout_left = 0.0f, layout_top = 0.0f;
    float                    layout_width = 0.0f, layout_height = 0.0f;
    bool                     emitted = false;
    bool                     shown = false;
    bool                     parked = false;
    Entity                   entity = 0;
    bool                     has_entity = false;

    const UITextData* TextData() const
    {
        if (const UITextData* text = std::get_if<UITextData>(&kind))    return text;
        if (const UIInputData* input = std::get_if<UIInputData>(&kind)) return &input->text;
        return nullptr;
    }
    UITextData* TextData() { return const_cast<UITextData*>(std::as_const(*this).TextData()); }

    bool HasRenderable() const { return draws && !quad.empty(); }
    bool ClipsChildren() const { return std::holds_alternative<UIBoxData>(kind) && style.Clips(); }
    bool Scrolls() const { return std::holds_alternative<UIBoxData>(kind) && style.Scrolls(); }
    UIContentBox ContentBox() const;
};

struct UILoopIndexReference {
    uint32_t depth = 0;
};

using UIKeyIndex = std::variant<uint32_t, UILoopIndexReference>;

struct UIVarReference {
    UIVarId                 var;
    std::vector<UIKeyIndex> key;
};

using UITemplatePart = std::variant<std::string, UIVarReference, UILoopIndexReference>;

struct UIDeclaration {
    std::string                 property;
    std::vector<UITemplatePart> value;
    uint32_t                    line = 0;
    UIMarkupSpan                span;
    bool                        from_style_attribute = false;
};

struct UIEdit {
    UIProperty  property = UIProperty::Display;
    std::string value;
};

struct UIElementStyle {
    std::string                 id;
    std::vector<std::string>    classes;
    std::vector<UIDeclaration>  declarations;
    uint32_t                    bound_properties = 0;
    std::vector<UIEdit>         edits;
    std::optional<UIMarkupSpan> style_value;
    uint32_t                    attributes_end = 0;
};

struct UIRootElement  { UIElementStyle style; std::vector<UIElementIndex> children; };
struct UIBoxElement   { UIElementStyle style; std::vector<UIElementIndex> children; };
struct UITextElement  { UIElementStyle style; std::vector<UITemplatePart> text; };
struct UIInputElement { UIElementStyle style; std::vector<UITemplatePart> text; };
struct UILoopElement  { std::vector<UITemplatePart> count; std::vector<UIElementIndex> children; };

using UIElementKind = std::variant<UIRootElement, UIBoxElement, UITextElement, UIInputElement, UILoopElement>;

struct UIElement {
    uint32_t      line = 0;
    UIElementKind kind;

    const UIElementStyle* Style() const
    {
        if (const UIRootElement* root = std::get_if<UIRootElement>(&kind))    return &root->style;
        if (const UIBoxElement* box = std::get_if<UIBoxElement>(&kind))       return &box->style;
        if (const UITextElement* text = std::get_if<UITextElement>(&kind))    return &text->style;
        if (const UIInputElement* input = std::get_if<UIInputElement>(&kind)) return &input->style;
        return nullptr;
    }
    UIElementStyle* Style() { return const_cast<UIElementStyle*>(std::as_const(*this).Style()); }

    const std::vector<UITemplatePart>* Text() const
    {
        if (const UITextElement* text = std::get_if<UITextElement>(&kind))    return &text->text;
        if (const UIInputElement* input = std::get_if<UIInputElement>(&kind)) return &input->text;
        return nullptr;
    }

    const std::vector<UIElementIndex>* Children() const
    {
        if (const UIRootElement* root = std::get_if<UIRootElement>(&kind)) return &root->children;
        if (const UIBoxElement* box = std::get_if<UIBoxElement>(&kind))    return &box->children;
        if (const UILoopElement* loop = std::get_if<UILoopElement>(&kind)) return &loop->children;
        return nullptr;
    }
    std::vector<UIElementIndex>* Children() { return const_cast<std::vector<UIElementIndex>*>(std::as_const(*this).Children()); }
};

struct UITextBinding  { UINodeIndex node; };
struct UIStyleBinding { UINodeIndex node; };
struct UICountBinding { UILoopIndex loop; };

using UIBindingTarget = std::variant<UITextBinding, UIStyleBinding, UICountBinding>;

struct UIBinding {
    UIBindingTarget target;
    UIElementIndex  element;
    UIVarKey        instance;
    bool            queued = false;
};

struct UIVarElement {
    std::string                 value;
    bool                        assigned = false;
    std::vector<UIBindingIndex> bindings;
};

struct UIVar {
    std::string                      default_value;
    std::map<UIVarKey, UIVarElement> elements;
};

struct UIVarCell { std::string name; std::unique_ptr<UIVar> object; };
using UIVarRegistry = ResourceRegistry<UIVarCell, UIVarId>;

struct UILoopInstance {
    std::vector<UISlot> slots;
};

struct UILoop {
    UIElementIndex              element;
    UINodeIndex                 parent;
    UIVarKey                    outer;
    uint32_t                    count = 0;
    bool                        parked = false;
    std::vector<UILoopInstance> instances;
};

class UI_Manager {
public:
    UI_Manager();
    ~UI_Manager();

    UINodeIndex Root(const UIStyle& style);
    UINodeIndex Box (UINodeIndex parent, const UIStyle& style, const std::string& material, const std::string& quad);
    UINodeIndex Text(UINodeIndex parent, const UIStyle& style, const std::string& utf8,
                     const std::string& material, const std::string& quad, FontId font, const FontManager* font_manager);
    UINodeIndex Input(UINodeIndex parent, const UIStyle& style, const std::string& utf8,
                      const std::string& material, const std::string& quad, FontId font, const FontManager* font_manager,
                      const std::string& caret_material, const std::string& caret_quad);

    bool Load(const FontManager* font_manager, const std::string& path);
    bool Save(const FontManager* font_manager);
    bool HasUnsavedChanges() const { return unsaved_edits > 0 || source_unsaved; }
    void Reset();

    UIVarId InternVar(const std::string& name);
    void    SetVar(UIVarId var, const UIVarKey& key, std::string_view value);
    void    SetVar(UIVarId var, std::string_view value) { SetVar(var, UIVarKey{}, value); }
    std::string GetVar(UIVarId var, const UIVarKey& key = {}) const;
    template <class Number> requires std::integral<Number> || std::floating_point<Number>
    void    SetVar(UIVarId var, const UIVarKey& key, Number value)
    {
        char buffer[32];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        SetVar(var, key, std::string_view(buffer, result.ptr));
    }
    template <class Number> requires std::integral<Number> || std::floating_point<Number>
    void    SetVar(UIVarId var, Number value) { SetVar(var, UIVarKey{}, value); }

    void ApplyDirtyBindings(EngineContext* ctx, const FontManager* font_manager, std::mutex& structure_mutex);
    void Emit(EngineContext* ctx, float screen_width, float screen_height);

    UINodeIndex RootNode() const;
    uint32_t    ChildCount(UINodeIndex node) const;
    UINodeIndex ChildAt(UINodeIndex node, uint32_t index) const;
    std::string NodeLabel(UINodeIndex node) const;
    UINodeIndex FindNode(std::string_view id, const UIVarKey& instance = {}) const;
    bool        IsNodeShown(UINodeIndex node) const;
    bool        NodeContains(UINodeIndex node, float window_x, float window_y) const;

    void NudgeNode(UINodeIndex node, float delta_x, float delta_y, float delta_z);
    bool GetNodeNdc(UINodeIndex node, float& ndc_x, float& ndc_y, float& ndc_z) const;
    void GetOffset(UINodeIndex node, float& offset_x, float& offset_y, float& offset_z) const;
    void SetNodeTransform(UINodeIndex node, const float matrix[16]);
    bool GetNodeTransform(UINodeIndex node, float matrix[16]) const;
    void SetNodeStyle(UINodeIndex node, const UIStyle& style);
    bool GetNodeStyle(UINodeIndex node, UIStyle& style) const;
    bool GetNodeLayoutSize(UINodeIndex node, float& width, float& height) const;
    bool IsTextNode(UINodeIndex node) const;
    bool IsLoopInstance(UINodeIndex node) const;
    uint32_t BoundProperties(UINodeIndex node) const;
    void SetText(UINodeIndex node, const std::string& utf8, const FontManager* font_manager);
    void SetNodeMaterial(EngineContext* ctx, UINodeIndex node, const std::string& material);
    bool GetNodeEntity(UINodeIndex node, Entity& entity) const;

    void EditNodeStyle(UINodeIndex node, const UIStyle& style);
    void EditNodeMaterial(EngineContext* ctx, UINodeIndex node, const std::string& material);

    void OnClick(const std::string& node_id, UIClickFunc fn);
    void PressAt(float window_x, float window_y);
    void ReleaseAt(float window_x, float window_y);
    void CancelPress();
    void ExecuteClicks();
    void SetMouseEnabled(bool enabled);
    bool WantCaptureMouse(float window_x, float window_y) const;
    void HandleInput(const std::vector<UIInputEvent>& events);
    void ClearFocus();
    bool WantCaptureKeyboard() const;
    void UpdateCaret(EngineContext* ctx);

private:
    void Clear();
    UINodeIndex MakeNode(const UIStyle& style);
    UINodeIndex MakeTextLeaf(const UIStyle& style, const std::string& material, const std::string& quad);
    void AttachChild(UINodeIndex parent, UINodeIndex child);
    void SetNodeParked(UINodeIndex node, bool parked);
    void RebuildChildren(UINodeIndex parent);
    void AppendSlotNodes(const std::vector<UISlot>& slots, std::vector<UINodeIndex>& out) const;

    std::string         PartValue(const UITemplatePart& part, const UIVarKey& instance) const;
    std::vector<UISlot> BuildItems(UIElementIndex element, UINodeIndex parent, const UIVarKey& instance, const FontManager* font_manager);
    UINodeIndex         BuildNode(UIElementIndex element, UINodeIndex parent, const UIVarKey& instance, const FontManager* font_manager);
    UILoopIndex         BuildLoop(UIElementIndex element, UINodeIndex parent, const UIVarKey& outer, const FontManager* font_manager);
    void                BuildInstances(UILoopIndex loop, uint32_t count, const FontManager* font_manager);
    bool                ReadLoopCount(UILoopIndex loop, uint32_t& count) const;
    void                ResizeLoop(UILoopIndex loop, uint32_t count, const FontManager* font_manager);
    void                UpdateParking(UILoopIndex loop);
    void                AddBinding(UIBinding binding);

    bool            LoadSource(const FontManager* font_manager, const std::string& path, const std::string& source);
    UIElementStyle* EditableStyle(UINodeIndex node);
    void            SetEdit(UIElementStyle& style, UIProperty property, std::string value, const std::string& file_value);
    std::string     SourceWithEdits() const;

    glm::vec2 NodePoint(UINodeIndex node, float window_x, float window_y) const;
    bool InsideClippers(UINodeIndex node, float window_x, float window_y) const;
    bool ContentContains(UINodeIndex node, float window_x, float window_y) const;
    bool IsInSubtree(UINodeIndex node, UINodeIndex ancestor) const;
    UIHit HitAt(float window_x, float window_y) const;
    UINodeIndex ClickableNodeAt(float window_x, float window_y) const;
    bool IsOverUI(float window_x, float window_y) const;
    bool IsInputNode(UINodeIndex node) const;
    bool InputVarOf(UINodeIndex node, UIVarId& var, UIVarKey& key) const;
    size_t InputPrefixLength(UINodeIndex node) const;
    size_t CaretAt(float window_x, float window_y) const;
    void SetFocus(UINodeIndex node);
    void FindHit(UINodeIndex node, UINodeIndex clickable_ancestor, float window_x, float window_y, UIHit& hit) const;
    void TypeIntoFocused(const std::vector<UIInputEvent>& events);

    UIScrollView*       ScrollViewOf(UINodeIndex node);
    const UIScrollView* ScrollViewOf(UINodeIndex node) const;
    glm::vec2           UpdateScrollView(UINodeIndex node);
    UINodeIndex         WheelTargetAt(const UIWheelInput& wheel) const;
    void                ScrollByWheel(const UIWheelInput& wheel);

    void DeleteCreatedEntities(EngineContext* ctx, const SceneName& scene_name);
    void EmitNode(EngineContext* ctx, ObjectManager* object_manager, SceneData* scene, const SceneName& scene_name,
                  UINodeIndex node, float parent_x, float parent_y, bool parent_shown, float screen_width, float screen_height,
                  const std::optional<UIClipArea>& clip, int& order, uint32_t& clip_count, bool create_entities);

    YGConfig*           yoga_config = nullptr;
    std::vector<UINode> nodes;
    std::vector<Entity> created_entities;
    UINodeIndex         root;
    float               last_screen_width = -1.0f, last_screen_height = -1.0f;
    bool                emit_needed = true;
    bool                recreate_needed = true;

    UIVarRegistry               variables;
    std::vector<UIElement>      elements;
    std::vector<UILoop>         loops;
    std::vector<UIBinding>      bindings;
    std::vector<UIBindingIndex> dirty_bindings;
    std::string                 document_path;
    std::string                 document_source;
    std::string                 disk_source;
    uint32_t                    unsaved_edits = 0;
    bool                        source_unsaved = false;

    std::unordered_map<std::string, UIClickFunc> click_instructions;
    std::vector<UIClick>                         pending_clicks;
    UINodeIndex                                  pressed_node;
    bool                                         press_on_ui = false;
    bool                                         mouse_enabled = true;
    UINodeIndex                                  focused_node;
    UIVarId                                      focused_var;
    UIVarKey                                     focused_key;
    size_t                                       focused_prefix = 0;
    size_t                                       caret = 0;
    UINodeIndex                                  caret_node;
    uint64_t                                     caret_blink_start = 0;

    std::vector<UIScrollView>                    scroll_views;
    UINodeIndex                                  wheel_target;
    glm::vec2                                    wheel_target_point{ 0.0f };
    uint64_t                                     wheel_target_release = 0;
    glm::vec2                                    wheel_average{ 0.0f };
};
