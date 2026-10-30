#pragma once
#include <charconv>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>
#include "Aliases.h"
#include "ComponentStorage.h"
#include "ResourceId.h"
#include "ResourceRegistry.h"

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
    float     grow        = 0.0f;
    UIEdges   padding;
    UIEdges   margin;
    float     gap         = 0.0f;
};

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

struct UINode {
    YGNode*                      yoga_node = nullptr;
    UIStyle                      style;
    bool                         draws = false;
    std::string                  material;
    std::string                  quad;
    std::unique_ptr<UITextShape> shape;
    std::vector<uint32_t>        wrapped_glyphs;
    FontId                       font;
    std::string                  id;
    std::vector<std::string>     classes;
    std::vector<uint32_t>        children;
    float                        offset_x = 0.0f, offset_y = 0.0f, offset_z = 0.0f;
    glm::mat4                    local{ 1.0f };
    float                        ndc_center_x = 0.0f, ndc_center_y = 0.0f, ndc_z = 0.5f;
    float                        layout_width = 0.0f, layout_height = 0.0f;
    bool                         emitted = false;
    bool                         shown = false;
    Entity                       entity = 0;
    bool                         has_entity = false;
};

struct UITemplatePart {
    std::string literal;
    UIVarId     var;
};

struct UIBoundDeclaration {
    std::string                 property;
    std::vector<UITemplatePart> value;
    uint32_t                    line = 0;
};

enum class UIBindingKind : uint8_t { Text, Style };

struct UIBinding {
    UIBindingKind                   kind = UIBindingKind::Text;
    uint32_t                        node = 0;
    std::vector<UITemplatePart>     text;
    std::vector<UIBoundDeclaration> declarations;
    bool                            queued = false;
};

struct UIVar {
    std::string           value;
    bool                  assigned = false;
    std::vector<uint32_t> bindings;
};

struct UIVarCell { std::string name; std::unique_ptr<UIVar> object; };
using UIVarRegistry = ResourceRegistry<UIVarCell, UIVarId>;

class UI_Manager {
public:
    UI_Manager();
    ~UI_Manager();

    using Node = uint32_t;
    static constexpr Node kInvalid = 0xFFFFFFFFu;

    Node Root(const UIStyle& style);
    Node Box (Node parent, const UIStyle& style, const std::string& material, const std::string& quad);
    Node Text(Node parent, const UIStyle& style, const std::string& utf8,
              const std::string& material, const std::string& quad, FontId font, const FontManager* font_manager);

    bool Load(const FontManager* font_manager, const std::string& path);
    void Reset();

    UIVarId InternVar(const std::string& name);
    void    SetVar(UIVarId var, std::string_view value);
    template <class Number> requires std::integral<Number> || std::floating_point<Number>
    void    SetVar(UIVarId var, Number value)
    {
        char buffer[32];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        SetVar(var, std::string_view(buffer, result.ptr));
    }

    void ApplyDirtyBindings(EngineContext* ctx, const FontManager* font_manager);
    void Emit(EngineContext* ctx, float screen_width, float screen_height);

    Node        RootNode() const;
    uint32_t    ChildCount(Node node) const;
    Node        ChildAt(Node node, uint32_t index) const;
    std::string NodeLabel(Node node) const;
    Node        FindNode(std::string_view id) const;
    bool        IsNodeShown(Node node) const;
    bool        NodeContains(Node node, float window_x, float window_y) const;

    void NudgeNode(Node node, float delta_x, float delta_y, float delta_z);
    bool GetNodeNdc(Node node, float& ndc_x, float& ndc_y, float& ndc_z) const;
    void GetOffset(Node node, float& offset_x, float& offset_y, float& offset_z) const;
    void SetNodeTransform(Node node, const float matrix[16]);
    bool GetNodeTransform(Node node, float matrix[16]) const;
    void SetNodeStyle(Node node, const UIStyle& style);
    bool GetNodeStyle(Node node, UIStyle& style) const;
    bool GetNodeLayoutSize(Node node, float& width, float& height) const;
    bool IsTextNode(Node node) const;
    void SetText(Node node, const std::string& utf8, const FontManager* font_manager);
    void SetNodeMaterial(EngineContext* ctx, Node node, const std::string& material);
    bool GetNodeEntity(Node node, Entity& entity) const;

private:
    void Clear();
    Node MakeNode(const UIStyle& style);
    void AttachChild(Node parent, Node child);
    void AddBinding(UIBinding binding);
    void DeleteCreatedEntities(EngineContext* ctx, const SceneName& scene_name);
    void EmitNode(EngineContext* ctx, ObjectManager* object_manager, SceneData* scene, const SceneName& scene_name,
                  Node node, float parent_x, float parent_y, bool parent_shown, float screen_width, float screen_height,
                  int& order, bool create_entities);

    YGConfig*           yoga_config = nullptr;
    std::vector<UINode> nodes;
    std::vector<Entity> created_entities;
    Node                root = kInvalid;
    float               last_screen_width = -1.0f, last_screen_height = -1.0f;
    bool                emit_needed = true;
    bool                recreate_needed = true;

    UIVarRegistry          variables;
    std::vector<UIBinding> bindings;
    std::vector<uint32_t>  dirty_bindings;
    std::string            document_path;
};
