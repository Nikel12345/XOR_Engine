#include "PCH.h"
#include "UI_Manager.h"
#include "EngineContext.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "FontManager.h"
#include <yoga/Yoga.h>

static void ApplyEdges(YGNodeRef yoga_node, void (*set_edge)(YGNodeRef, YGEdge, float), const UIEdges& edges)
{
    set_edge(yoga_node, YGEdgeTop,    edges.top);
    set_edge(yoga_node, YGEdgeRight,  edges.right);
    set_edge(yoga_node, YGEdgeBottom, edges.bottom);
    set_edge(yoga_node, YGEdgeLeft,   edges.left);
}

static void ApplyLimit(YGNodeRef yoga_node, void (*set_points)(YGNodeRef, float), void (*set_percent)(YGNodeRef, float),
                       UILimit mode, float limit)
{
    switch (mode) {
        case UILimit::Points:  set_points(yoga_node, limit);       break;
        case UILimit::Percent: set_percent(yoga_node, limit);      break;
        default:               set_points(yoga_node, YGUndefined); break;
    }
}

static void ApplyStyle(YGNodeRef yoga_node, const UIStyle& style, bool parked)
{
    YGNodeStyleSetDisplay(yoga_node, style.display == UIDisplay::None || parked ? YGDisplayNone : YGDisplayFlex);
    YGNodeStyleSetFlexDirection(yoga_node, style.direction == UIDir::Row ? YGFlexDirectionRow : YGFlexDirectionColumn);

    switch (style.justify) {
        case UIJustify::Start:        YGNodeStyleSetJustifyContent(yoga_node, YGJustifyFlexStart);    break;
        case UIJustify::Center:       YGNodeStyleSetJustifyContent(yoga_node, YGJustifyCenter);       break;
        case UIJustify::End:          YGNodeStyleSetJustifyContent(yoga_node, YGJustifyFlexEnd);      break;
        case UIJustify::SpaceBetween: YGNodeStyleSetJustifyContent(yoga_node, YGJustifySpaceBetween); break;
    }
    switch (style.align) {
        case UIAlign::Start:   YGNodeStyleSetAlignItems(yoga_node, YGAlignFlexStart); break;
        case UIAlign::Center:  YGNodeStyleSetAlignItems(yoga_node, YGAlignCenter);    break;
        case UIAlign::End:     YGNodeStyleSetAlignItems(yoga_node, YGAlignFlexEnd);   break;
        case UIAlign::Stretch: YGNodeStyleSetAlignItems(yoga_node, YGAlignStretch);   break;
    }

    // Каждое поле пишется явно, и умолчание тоже: стиль применяется повторно к живому узлу
    // (SetNodeStyle), и поле, сброшенное в Auto или 0, обязано сброситься и в Yoga.
    switch (style.width_mode) {
        case UISize::Points:  YGNodeStyleSetWidth(yoga_node, style.width);        break;
        case UISize::Percent: YGNodeStyleSetWidthPercent(yoga_node, style.width); break;
        default:              YGNodeStyleSetWidthAuto(yoga_node);                 break;
    }
    switch (style.height_mode) {
        case UISize::Points:  YGNodeStyleSetHeight(yoga_node, style.height);        break;
        case UISize::Percent: YGNodeStyleSetHeightPercent(yoga_node, style.height); break;
        default:              YGNodeStyleSetHeightAuto(yoga_node);                  break;
    }
    ApplyLimit(yoga_node, YGNodeStyleSetMinWidth,  YGNodeStyleSetMinWidthPercent,  style.min_width_mode,  style.min_width);
    ApplyLimit(yoga_node, YGNodeStyleSetMaxWidth,  YGNodeStyleSetMaxWidthPercent,  style.max_width_mode,  style.max_width);
    ApplyLimit(yoga_node, YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent, style.min_height_mode, style.min_height);
    ApplyLimit(yoga_node, YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent, style.max_height_mode, style.max_height);

    YGNodeStyleSetFlexGrow(yoga_node, style.FlexGrow());
    YGNodeStyleSetFlexShrink(yoga_node, style.shrink);
    ApplyEdges(yoga_node, YGNodeStyleSetPadding, style.padding);
    ApplyEdges(yoga_node, YGNodeStyleSetMargin,  style.margin);
    YGNodeStyleSetGap(yoga_node, YGGutterAll, style.gap);

    // YGOverflowHidden Yoga раскладывает как visible: текст в полосе сжался бы под её ширину и
    // перенёсся. Детей по содержимому вдоль главной оси меряет только YGOverflowScroll.
    const UIOverflow main_axis_overflow = style.direction == UIDir::Row ? style.overflow_x : style.overflow_y;
    YGNodeStyleSetOverflow(yoga_node, main_axis_overflow == UIOverflow::Visible ? YGOverflowVisible : YGOverflowScroll);
}

static std::vector<UITextShape::Glyph> ShapeGlyphs(const FontData* font, const FontManager* font_manager, const std::string& utf8)
{
    std::vector<UITextShape::Glyph> glyphs;
    for (uint32_t code : font_manager->ShapeString(font, utf8)) {
        const GlyphInfo* glyph = code < font->glyphs.size() ? &font->glyphs[code] : nullptr;
        glyphs.push_back({ code, glyph ? safe_sint32_f(glyph->advance) : 0.0f, glyph && glyph->codepoint == ' ' });
    }
    return glyphs;
}

struct WrappedSize {
    float width = 0.0f;
    int   lines = 1;
};

// wrapped == nullptr — только замер.
static WrappedSize WrapText(const UITextShape& shape, float max_width, std::vector<uint32_t>* wrapped,
                            std::vector<UIGlyphPlace>* places)
{
    const std::vector<UITextShape::Glyph>& glyphs = shape.glyphs;
    WrappedSize size;
    float line_width    = 0.0f;
    float pen           = 0.0f;
    bool  line_has_word = false;
    const auto place = [&](size_t index) {
        if (places) places->push_back({ safe_u32(wrapped->size()), size.lines - 1, pen });
        pen += glyphs[index].advance;
    };
    const auto write = [&](size_t index) {
        place(index);
        if (wrapped) wrapped->push_back(glyphs[index].code);
    };

    size_t i = 0;
    while (i < glyphs.size()) {
        if (glyphs[i].code == UI_LINE_BREAK_CODE) {
            size.width    = std::max(size.width, line_width);
            line_width    = 0.0f;
            line_has_word = false;
            write(i);
            pen = 0.0f;
            ++size.lines;
            ++i;
            continue;
        }

        const size_t spaces_begin = i;
        float spaces_width = 0.0f;
        for (; i < glyphs.size() && glyphs[i].space; ++i) spaces_width += glyphs[i].advance;
        const size_t word_begin = i;
        float word_width = 0.0f;
        for (; i < glyphs.size() && !glyphs[i].space && glyphs[i].code != UI_LINE_BREAK_CODE; ++i)
            word_width += glyphs[i].advance;
        if (word_begin == i) {
            for (size_t k = spaces_begin; k < word_begin; ++k) place(k);
            continue;
        }

        if (line_has_word && line_width + spaces_width + word_width > max_width) {
            for (size_t k = spaces_begin; k < word_begin; ++k) place(k);
            size.width = std::max(size.width, line_width);
            line_width = word_width;
            pen        = 0.0f;
            ++size.lines;
            if (wrapped) wrapped->push_back(UI_LINE_BREAK_CODE);
        } else {
            line_width += spaces_width + word_width;
            for (size_t k = spaces_begin; k < word_begin; ++k) write(k);
        }
        for (size_t k = word_begin; k < i; ++k) write(k);
        line_has_word = true;
    }
    size.width = std::max(size.width, line_width);
    if (places) places->push_back({ safe_u32(wrapped->size()), size.lines - 1, pen });
    return size;
}

static YGSize MeasureText(YGNodeConstRef yoga_node, float width, YGMeasureMode width_mode, float, YGMeasureMode)
{
    const UITextShape& shape = *static_cast<const UITextShape*>(YGNodeGetContext(yoga_node));
    const float max_width = width_mode == YGMeasureModeUndefined ? std::numeric_limits<float>::infinity() : width;
    const WrappedSize size = WrapText(shape, max_width, nullptr, nullptr);
    return { size.width, safe_sint32_f(size.lines) * shape.line_height };
}

static void WritePositions(ObjectManager* object_manager, SceneData* scene, Entity entity, const PositionProxy16& matrix)
{
    SoAElement<Positions> element = object_manager->GetComponent<Positions>(scene, entity);
    Positions& columns = element.container();
    const size_t row = element.i();
    columns.x[row] = matrix.x;  columns.y[row] = matrix.y;  columns.z[row] = matrix.z;  columns.w[row] = matrix.w;
    columns.a[row] = matrix.a;  columns.b[row] = matrix.b;  columns.c[row] = matrix.c;  columns.d[row] = matrix.d;
    columns.e[row] = matrix.e;  columns.f[row] = matrix.f;  columns.g[row] = matrix.g;  columns.h[row] = matrix.h;
    columns.i[row] = matrix.i;  columns.j[row] = matrix.j;  columns.k[row] = matrix.k;  columns.l[row] = matrix.l;
}

static UITextComponent MakeTextComponent(const UITextData& text, float width, float height)
{
    const float line_height = text.shape->line_height;
    return UITextComponent{ text.wrapped_glyphs, text.font, width / line_height, height / line_height };
}

UIContentBox UINode::ContentBox() const
{
    return UIContentBox{ layout_left + YGNodeLayoutGetPadding(yoga_node, YGEdgeLeft),
                         layout_top  + YGNodeLayoutGetPadding(yoga_node, YGEdgeTop),
                         layout_left + layout_width  - YGNodeLayoutGetPadding(yoga_node, YGEdgeRight),
                         layout_top  + layout_height - YGNodeLayoutGetPadding(yoga_node, YGEdgeBottom) };
}

static UIClipRectComponent MakeClipRect(const UIClipArea& area, float screen_width, float screen_height)
{
    return UIClipRectComponent{ area.index,
                                area.left  / screen_width * 2.0f - 1.0f, 1.0f - area.top    / screen_height * 2.0f,
                                area.right / screen_width * 2.0f - 1.0f, 1.0f - area.bottom / screen_height * 2.0f };
}

template <class... Components>
static Entity CreateWithClipRect(EngineContext* ctx, const SceneName& scene_name,
                                 const std::optional<UIClipRectComponent>& clip_rect, Components&&... components)
{
    if (clip_rect) return ctx->CreateEntity(scene_name, UIClipRectComponent{ *clip_rect }, std::forward<Components>(components)...);
    return ctx->CreateEntity(scene_name, std::forward<Components>(components)...);
}

template <class... Components>
static Entity CreateNodeEntity(EngineContext* ctx, const SceneName& scene_name, const std::optional<UIClipComponent>& clip,
                               const std::optional<UIClipRectComponent>& clip_rect, Components&&... components)
{
    if (clip)
        return CreateWithClipRect(ctx, scene_name, clip_rect, UIClipComponent{ *clip }, std::forward<Components>(components)...);
    return CreateWithClipRect(ctx, scene_name, clip_rect, std::forward<Components>(components)...);
}

static void WriteClip(ObjectManager* object_manager, SceneData* scene, Entity entity, const std::optional<UIClipComponent>& clip,
                      const std::optional<UIClipRectComponent>& clip_rect)
{
    if (clip && object_manager->Has<UIClipComponent>(scene, entity))
        object_manager->GetComponent<UIClipComponent>(scene, entity) = *clip;
    if (clip_rect && object_manager->Has<UIClipRectComponent>(scene, entity))
        object_manager->GetComponent<UIClipRectComponent>(scene, entity) = *clip_rect;
}

static const char* KindName(const UINodeKind& kind)
{
    if (std::holds_alternative<UIBoxData>(kind))   return "box";
    if (std::holds_alternative<UITextData>(kind))  return "text";
    if (std::holds_alternative<UIInputData>(kind)) return "input";
    assert(false && "KindName: node of unknown kind");
    return "?";
}

static std::optional<UITextData> MakeTextData(YGNodeRef yoga_node, const std::string& utf8, FontId font,
                                              const FontManager* font_manager)
{
    const FontData* font_data = font_manager ? font_manager->GetFont(font) : nullptr;
    if (!font_data) return std::nullopt;
    UITextData text{ std::make_unique<UITextShape>(), font };
    text.shape->line_height = safe_sint32_f(font_data->line_height);
    text.shape->glyphs      = ShapeGlyphs(font_data, font_manager, utf8);
    // Адрес shape — контекст measure-функции, поэтому shape живёт отдельной аллокацией: nodes
    // переезжает при росте.
    YGNodeSetContext(yoga_node, text.shape.get());
    YGNodeSetMeasureFunc(yoga_node, MeasureText);
    return text;
}

UI_Manager::UI_Manager() : yoga_config(YGConfigNew()) {}

UI_Manager::~UI_Manager()
{
    Clear();
    if (yoga_config) YGConfigFree(yoga_config);
}

UINodeIndex UI_Manager::RootNode() const { return root; }

uint32_t UI_Manager::ChildCount(UINodeIndex node) const
{
    return node.value < nodes.size() ? safe_u32(nodes[node.value].children.size()) : 0u;
}

UINodeIndex UI_Manager::ChildAt(UINodeIndex node, uint32_t index) const
{
    if (node.value >= nodes.size() || index >= nodes[node.value].children.size()) return {};
    return nodes[node.value].children[index];
}

std::string UI_Manager::NodeLabel(UINodeIndex node) const
{
    if (node.value >= nodes.size()) return "<invalid>";
    const UINode& entry = nodes[node.value];
    std::string label = node == root ? "ui" : KindName(entry.kind);
    if (!entry.id.empty()) label += "#" + entry.id;
    for (const std::string& name : entry.classes) label += "." + name;
    for (uint32_t index : entry.instance) label += "[" + std::to_string(index) + "]";
    return label;
}

UINodeIndex UI_Manager::FindNode(std::string_view id, const UIVarKey& instance) const
{
    if (id.empty()) return {};
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id && nodes[i].instance == instance) return UINodeIndex{ safe_u32(i) };
    return {};
}

bool UI_Manager::IsNodeShown(UINodeIndex node) const
{
    return node.value < nodes.size() && nodes[node.value].emitted && nodes[node.value].shown;
}

glm::vec2 UI_Manager::NodePoint(UINodeIndex node, float window_x, float window_y) const
{
    const UINode& entry = nodes[node.value];
    const float center_x = (entry.ndc_center_x + 1.0f) * 0.5f * last_screen_width;
    const float center_y = (1.0f - entry.ndc_center_y) * 0.5f * last_screen_height;
    // local работает в пикселях с осью y вверх (как NDC), а окно считает y вниз.
    return glm::inverse(glm::mat2(entry.local)) * glm::vec2(window_x - center_x, center_y - window_y);
}

bool UI_Manager::InsideClippers(UINodeIndex node, float window_x, float window_y) const
{
    for (UINodeIndex ancestor = nodes[node.value].parent; ancestor; ancestor = nodes[ancestor.value].parent) {
        const UINode& clipper = nodes[ancestor.value];
        if (!clipper.ClipsChildren()) continue;
        const UIContentBox content = clipper.ContentBox();
        if (clipper.style.overflow_x != UIOverflow::Visible && (window_x < content.left || window_x > content.right))
            return false;
        if (clipper.style.overflow_y != UIOverflow::Visible && (window_y < content.top || window_y > content.bottom))
            return false;
    }
    return true;
}

bool UI_Manager::NodeContains(UINodeIndex node, float window_x, float window_y) const
{
    if (!IsNodeShown(node)) return false;
    const UINode& entry = nodes[node.value];
    if (glm::determinant(glm::mat2(entry.local)) == 0.0f) return false;
    if (!InsideClippers(node, window_x, window_y)) return false;

    const glm::vec2 point = NodePoint(node, window_x, window_y);
    return std::abs(point.x) <= 0.5f * entry.layout_width && std::abs(point.y) <= 0.5f * entry.layout_height;
}

bool UI_Manager::ContentContains(UINodeIndex node, float window_x, float window_y) const
{
    return IsNodeShown(node) && InsideClippers(node, window_x, window_y) && nodes[node.value].ContentBox().Contains(window_x, window_y);
}

bool UI_Manager::IsInSubtree(UINodeIndex node, UINodeIndex ancestor) const
{
    for (; node; node = nodes[node.value].parent)
        if (node == ancestor) return true;
    return false;
}

void UI_Manager::NudgeNode(UINodeIndex node, float delta_x, float delta_y, float delta_z)
{
    if (node.value >= nodes.size()) return;
    UINode& entry = nodes[node.value];
    entry.offset_x += delta_x;
    entry.offset_y += delta_y;
    entry.offset_z += delta_z;
    emit_needed = true;
}

bool UI_Manager::GetNodeNdc(UINodeIndex node, float& ndc_x, float& ndc_y, float& ndc_z) const
{
    if (!IsNodeShown(node)) return false;
    const UINode& entry = nodes[node.value];
    ndc_x = entry.ndc_center_x;
    ndc_y = entry.ndc_center_y;
    ndc_z = entry.ndc_z;
    return true;
}

void UI_Manager::GetOffset(UINodeIndex node, float& offset_x, float& offset_y, float& offset_z) const
{
    if (node.value >= nodes.size()) { offset_x = offset_y = offset_z = 0.0f; return; }
    const UINode& entry = nodes[node.value];
    offset_x = entry.offset_x;
    offset_y = entry.offset_y;
    offset_z = entry.offset_z;
}

void UI_Manager::SetNodeTransform(UINodeIndex node, const float matrix[16])
{
    if (node.value >= nodes.size()) return;
    nodes[node.value].local = glm::make_mat4(matrix);
    emit_needed = true;
}

bool UI_Manager::GetNodeTransform(UINodeIndex node, float matrix[16]) const
{
    if (node.value >= nodes.size()) return false;
    std::memcpy(matrix, glm::value_ptr(nodes[node.value].local), 16 * sizeof(float));
    return true;
}

void UI_Manager::SetNodeStyle(UINodeIndex node, const UIStyle& style)
{
    if (node.value >= nodes.size()) return;
    UINode& entry = nodes[node.value];
    const bool clipped_children = entry.ClipsChildren();
    entry.style = style;
    if (entry.TextData()) {
        entry.style.padding    = {};
        entry.style.overflow_x = UIOverflow::Visible;
        entry.style.overflow_y = UIOverflow::Visible;
    }
    ApplyStyle(entry.yoga_node, entry.style, entry.parked);
    emit_needed = true;
    // Рамка обрезки — компонент энтити контейнера и его потомков, а переноса энтити между
    // архетипами нет: компонент появляется и пропадает только с пересозданием.
    if (entry.ClipsChildren() != clipped_children) recreate_needed = true;
    if (!entry.Scrolls())
        std::erase_if(scroll_views, [node](const UIScrollView& view) { return view.node == node; });
}

void UI_Manager::SetNodeParked(UINodeIndex node, bool parked)
{
    UINode& entry = nodes[node.value];
    if (entry.parked == parked) return;
    entry.parked = parked;
    ApplyStyle(entry.yoga_node, entry.style, parked);
    emit_needed = true;
    if (parked)
        std::erase_if(scroll_views, [this, node](const UIScrollView& view) { return IsInSubtree(view.node, node); });
}

UIScrollView* UI_Manager::ScrollViewOf(UINodeIndex node)
{
    return const_cast<UIScrollView*>(std::as_const(*this).ScrollViewOf(node));
}

const UIScrollView* UI_Manager::ScrollViewOf(UINodeIndex node) const
{
    for (const UIScrollView& view : scroll_views)
        if (view.node == node) return &view;
    return nullptr;
}

glm::vec2 UI_Manager::UpdateScrollView(UINodeIndex node)
{
    UIScrollView* view = ScrollViewOf(node);
    if (!view) view = &scroll_views.emplace_back(UIScrollView{ node });

    const UINode& entry = nodes[node.value];
    // Yoga обнуляет раскладку поддерева с display: none, и запас скрытого узла стал бы нулём вместе с
    // позицией: после показа список стоял бы в начале.
    if (entry.shown) {
        float content_right = 0.0f, content_bottom = 0.0f;
        for (UINodeIndex child : entry.children) {
            const YGNodeConstRef yoga_child = nodes[child.value].yoga_node;
            if (YGNodeStyleGetDisplay(yoga_child) == YGDisplayNone) continue;
            content_right  = std::max(content_right,  YGNodeLayoutGetLeft(yoga_child) + YGNodeLayoutGetWidth(yoga_child) +
                                                      YGNodeLayoutGetMargin(yoga_child, YGEdgeRight));
            content_bottom = std::max(content_bottom, YGNodeLayoutGetTop(yoga_child) + YGNodeLayoutGetHeight(yoga_child) +
                                                      YGNodeLayoutGetMargin(yoga_child, YGEdgeBottom));
        }
        const float visible_right  = entry.layout_width  - YGNodeLayoutGetPadding(entry.yoga_node, YGEdgeRight);
        const float visible_bottom = entry.layout_height - YGNodeLayoutGetPadding(entry.yoga_node, YGEdgeBottom);
        view->range_x  = entry.style.ScrollsX() ? std::max(content_right  - visible_right,  0.0f) : 0.0f;
        view->range_y  = entry.style.ScrollsY() ? std::max(content_bottom - visible_bottom, 0.0f) : 0.0f;
        view->scroll_x = std::clamp(view->scroll_x, 0.0f, view->range_x);
        view->scroll_y = std::clamp(view->scroll_y, 0.0f, view->range_y);
    }
    // Yoga кладёт узлы на целые пиксели, а колесо тачпада даёт дробный сдвиг: текст между пикселями
    // размылся бы.
    return glm::vec2(std::round(view->scroll_x), std::round(view->scroll_y));
}

void UI_Manager::RebuildChildren(UINodeIndex parent)
{
    std::vector<UINodeIndex> order;
    AppendSlotNodes(nodes[parent.value].slots, order);
    std::vector<YGNodeRef> yoga_children;
    yoga_children.reserve(order.size());
    for (UINodeIndex child : order) yoga_children.push_back(nodes[child.value].yoga_node);
    YGNodeSetChildren(nodes[parent.value].yoga_node, yoga_children.data(), yoga_children.size());
    nodes[parent.value].children = std::move(order);
    emit_needed = true;
}

bool UI_Manager::GetNodeStyle(UINodeIndex node, UIStyle& style) const
{
    if (node.value >= nodes.size()) return false;
    style = nodes[node.value].style;
    return true;
}

bool UI_Manager::GetNodeLayoutSize(UINodeIndex node, float& width, float& height) const
{
    if (node.value >= nodes.size() || !nodes[node.value].emitted) return false;
    width  = nodes[node.value].layout_width;
    height = nodes[node.value].layout_height;
    return true;
}

bool UI_Manager::IsTextNode(UINodeIndex node) const
{
    return node.value < nodes.size() && nodes[node.value].TextData() != nullptr;
}

bool UI_Manager::IsLoopInstance(UINodeIndex node) const
{
    return node.value < nodes.size() && !nodes[node.value].instance.empty();
}

uint32_t UI_Manager::BoundProperties(UINodeIndex node) const
{
    if (node.value >= nodes.size() || nodes[node.value].element.value >= elements.size()) return 0;
    const UIElementStyle* style = elements[nodes[node.value].element.value].Style();
    return style ? style->bound_properties : 0;
}

void UI_Manager::SetText(UINodeIndex node, const std::string& utf8, const FontManager* font_manager)
{
    if (node.value >= nodes.size() || !font_manager) return;
    UINode& entry = nodes[node.value];
    UITextData* text = entry.TextData();
    const FontData* font = text ? font_manager->GetFont(text->font) : nullptr;
    if (!font) return;

    std::vector<UITextShape::Glyph> glyphs = ShapeGlyphs(font, font_manager, utf8);
    if (glyphs == text->shape->glyphs) return;
    text->shape->glyphs = std::move(glyphs);
    // Yoga кэширует замер узла с measure-функцией и без пометки не спросит размер заново.
    YGNodeMarkDirty(entry.yoga_node);
    emit_needed = true;
}

void UI_Manager::SetNodeMaterial(EngineContext* ctx, UINodeIndex node, const std::string& material)
{
    if (node.value >= nodes.size()) return;
    UINode& entry = nodes[node.value];
    if (entry.material == material || (entry.TextData() && material.empty())) return;
    entry.material = material;

    const bool draws = entry.TextData() || !material.empty();
    if (draws != entry.draws) {
        entry.draws     = draws;
        emit_needed     = true;
        recreate_needed = true;
    }
    else if (entry.has_entity) {
        ctx->ChangeMaterial(entry.entity, material);
    }
}

bool UI_Manager::GetNodeEntity(UINodeIndex node, Entity& entity) const
{
    if (node.value >= nodes.size() || !nodes[node.value].has_entity) return false;
    entity = nodes[node.value].entity;
    return true;
}

UINodeIndex UI_Manager::MakeNode(const UIStyle& style)
{
    YGNodeRef yoga_node = YGNodeNewWithConfig(yoga_config);
    ApplyStyle(yoga_node, style, false);
    const UINodeIndex node{ safe_u32(nodes.size()) };
    nodes.push_back(UINode{ yoga_node, style });   // может реаллоцировать: ссылки на узлы — только после него
    return node;
}

void UI_Manager::AttachChild(UINodeIndex parent, UINodeIndex child)
{
    UINode& parent_entry = nodes[parent.value];
    YGNodeInsertChild(parent_entry.yoga_node, nodes[child.value].yoga_node, YGNodeGetChildCount(parent_entry.yoga_node));
    parent_entry.children.push_back(child);
    nodes[child.value].parent = parent;
    emit_needed     = true;
    recreate_needed = true;
}

UINodeIndex UI_Manager::Root(const UIStyle& style)
{
    Clear();
    root = MakeNode(style);
    return root;
}

UINodeIndex UI_Manager::Box(UINodeIndex parent, const UIStyle& style, const std::string& material, const std::string& quad)
{
    const UINodeIndex node = MakeNode(style);
    UINode& entry = nodes[node.value];
    entry.material = material;
    entry.quad     = quad;
    entry.draws    = !material.empty();
    AttachChild(parent, node);
    return node;
}

UINodeIndex UI_Manager::MakeTextLeaf(const UIStyle& style, const std::string& material, const std::string& quad)
{
    // ui.frag рисует текст от угла рамки узла и об отступе внутри неё не знает.
    UIStyle text_style = style;
    if (text_style.padding != UIEdges{}) {
        SDL_Log("UI_Manager::Text: padding belongs to the parent box, ignored");
        text_style.padding = {};
    }
    if (text_style.Clips()) {
        SDL_Log("UI_Manager::Text: overflow belongs to the parent box, ignored");
        text_style.overflow_x = UIOverflow::Visible;
        text_style.overflow_y = UIOverflow::Visible;
    }
    const UINodeIndex node = MakeNode(text_style);
    UINode& entry = nodes[node.value];
    entry.material = material;
    entry.quad     = quad;
    entry.draws    = true;
    return node;
}

UINodeIndex UI_Manager::Text(UINodeIndex parent, const UIStyle& style, const std::string& utf8,
                             const std::string& material, const std::string& quad, FontId font,
                             const FontManager* font_manager)
{
    const UINodeIndex node = MakeTextLeaf(style, material, quad);
    if (std::optional<UITextData> text = MakeTextData(nodes[node.value].yoga_node, utf8, font, font_manager))
        nodes[node.value].kind = std::move(*text);
    AttachChild(parent, node);
    return node;
}

UINodeIndex UI_Manager::Input(UINodeIndex parent, const UIStyle& style, const std::string& utf8,
                              const std::string& material, const std::string& quad, FontId font,
                              const FontManager* font_manager, const std::string& caret_material,
                              const std::string& caret_quad)
{
    const UINodeIndex node = MakeTextLeaf(style, material, quad);
    if (std::optional<UITextData> text = MakeTextData(nodes[node.value].yoga_node, utf8, font, font_manager))
        nodes[node.value].kind = UIInputData{ std::move(*text), caret_material, caret_quad };
    AttachChild(parent, node);
    return node;
}

void UI_Manager::Clear()
{
    // Box и Text всегда вставляют узел в родителя, поэтому free корня освобождает всё дерево.
    if (root && nodes[root.value].yoga_node) YGNodeFreeRecursive(nodes[root.value].yoga_node);
    nodes.clear();
    root            = {};
    pressed_node    = {};
    focused_node    = {};
    caret_node      = {};
    wheel_target    = {};
    emit_needed     = true;
    recreate_needed = true;
    scroll_views.clear();

    elements.clear();
    unsaved_edits  = 0;
    source_unsaved = false;
    loops.clear();
    bindings.clear();
    dirty_bindings.clear();
    for (int32_t i = 0; i < variables.Count(); ++i) {
        UIVar* var = variables.Get(UIVarId{ i });
        if (!var) continue;
        for (auto& entry : var->elements) entry.second.bindings.clear();
        std::erase_if(var->elements, [](const auto& entry) { return !entry.second.assigned; });
    }
    document_path.clear();
}

void UI_Manager::Reset()
{
    Clear();
    document_source.clear();
    disk_source.clear();
    created_entities.clear();   // энтити не удаляем: их уже нет, id принадлежат новой сцене
    click_instructions.clear();
    pending_clicks.clear();
}

void UI_Manager::DeleteCreatedEntities(EngineContext* ctx, const SceneName& scene_name)
{
    for (Entity entity : created_entities) ctx->DeleteEntity(scene_name, entity);
    created_entities.clear();
}

void UI_Manager::EmitNode(EngineContext* ctx, ObjectManager* object_manager, SceneData* scene, const SceneName& scene_name,
                          UINodeIndex node, float parent_x, float parent_y, bool parent_shown, float screen_width, float screen_height,
                          const std::optional<UIClipArea>& clip, int& order, uint32_t& clip_count, bool create_entities)
{
    constexpr float kZStep = 0.00001f;

    UINode& entry = nodes[node.value];
    const float left   = parent_x + YGNodeLayoutGetLeft(entry.yoga_node) + entry.offset_x;
    const float top    = parent_y + YGNodeLayoutGetTop(entry.yoga_node)  + entry.offset_y;
    const float width  = YGNodeLayoutGetWidth(entry.yoga_node);
    const float height = YGNodeLayoutGetHeight(entry.yoga_node);
    const float ndc_z  = 0.5f - safe_sint32_f(order++) * kZStep + entry.offset_z;

    const glm::mat3 to_ndc(glm::vec3(2.0f / screen_width, 0.0f, 0.0f),
                           glm::vec3(0.0f, 2.0f / screen_height, 0.0f),
                           glm::vec3(0.0f, 0.0f, 2.0f / screen_height));
    const glm::mat3 size(glm::vec3(width, 0.0f, 0.0f), glm::vec3(0.0f, height, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat3 basis = to_ndc * glm::mat3(entry.local) * size;
    const float ndc_x = ((left + 0.5f * width) / screen_width) * 2.0f - 1.0f + entry.local[3][0] * 2.0f / screen_width;
    const float ndc_y = 1.0f - ((top + 0.5f * height) / screen_height) * 2.0f + entry.local[3][1] * 2.0f / screen_height;

    glm::mat4 transform(basis);
    transform[3] = glm::vec4(ndc_x, ndc_y, ndc_z, 1.0f);
    entry.ndc_center_x  = ndc_x;
    entry.ndc_center_y  = ndc_y;
    entry.ndc_z         = ndc_z;
    entry.layout_left   = left;
    entry.layout_top    = top;
    entry.layout_width  = width;
    entry.layout_height = height;
    entry.emitted       = true;
    entry.shown         = parent_shown && entry.style.display != UIDisplay::None && !entry.parked;

    UITextData* text = entry.TextData();
    if (text) {
        // Yoga округляет раскладку до пикселя, и ширина узла расходится с той, по которой мерил
        // MeasureText, на долю пикселя: без запаса строка, влезшая при замере, здесь перенеслась бы.
        constexpr float kWrapSlack = 1.0f;
        text->wrapped_glyphs.clear();
        text->glyph_places.clear();
        WrapText(*text->shape, width + kWrapSlack, &text->wrapped_glyphs, &text->glyph_places);
    }

    std::optional<UIClipArea>          children_clip = clip;
    std::optional<UIClipRectComponent> clip_rect;
    if (entry.ClipsChildren()) {
        const UIContentBox content = entry.ContentBox();
        UIClipArea area = clip.value_or(UIClipArea{ 0, 0.0f, 0.0f, screen_width, screen_height });
        area.index = clip_count++;
        if (entry.style.overflow_x != UIOverflow::Visible) {
            area.left  = std::max(area.left,  content.left);
            area.right = std::min(area.right, content.right);
        }
        if (entry.style.overflow_y != UIOverflow::Visible) {
            area.top    = std::max(area.top,    content.top);
            area.bottom = std::min(area.bottom, content.bottom);
        }
        children_clip = area;
        clip_rect     = MakeClipRect(area, screen_width, screen_height);
    }
    const glm::vec2 scroll = entry.Scrolls() ? UpdateScrollView(node) : glm::vec2(0.0f);

    const bool renders = entry.HasRenderable();
    std::optional<UIClipComponent> drawn_clip;
    if (renders && clip) drawn_clip = UIClipComponent{ clip->index };
    const PositionProxy16 positions{ transform[0][0], transform[0][1], transform[0][2], transform[3][0],
                                     transform[1][0], transform[1][1], transform[1][2], transform[3][1],
                                     transform[2][0], transform[2][1], transform[2][2], transform[3][2],
                                     transform[0][3], transform[1][3], transform[2][3], transform[3][3] };
    UIInputData* input = std::get_if<UIInputData>(&entry.kind);

    if (create_entities) {
        // GeneratedComponent: узлы пересоздаёт Emit, их копия в scene.sheaf дала бы дубли при загрузке.
        // TextureStateComponent ставится всегда: тег вешается только при создании, а материал узлу
        // могут сменить на вариативный позже.
        if (!renders)
            entry.entity = CreateNodeEntity(ctx, scene_name, std::nullopt, clip_rect, UIComponent{}, GeneratedComponent{});
        else if (text)
            entry.entity = CreateNodeEntity(ctx, scene_name, drawn_clip, clip_rect,
                RenderableProxy::Single(ctx->GetModelManager()->InternModel(entry.quad),
                                        { ctx->GetMaterialManager()->InternMaterial(entry.material) }, entry.shown),
                positions, UIComponent{}, GeneratedComponent{}, TextureStateComponent{},
                MakeTextComponent(*text, width, height));
        else
            entry.entity = CreateNodeEntity(ctx, scene_name, drawn_clip, clip_rect,
                RenderableProxy::Single(ctx->GetModelManager()->InternModel(entry.quad),
                                        { ctx->GetMaterialManager()->InternMaterial(entry.material) }, entry.shown),
                positions, UIComponent{}, GeneratedComponent{}, TextureStateComponent{});
        entry.has_entity = true;
        created_entities.push_back(entry.entity);
        // Каретку удаляет каскад вместе с полем: в created_entities её удалили бы дважды.
        if (renders && input)
            input->caret_entity = CreateNodeEntity(ctx, scene_name, drawn_clip, std::nullopt,
                RenderableProxy::Single(ctx->GetModelManager()->InternModel(input->caret_quad),
                                        { ctx->GetMaterialManager()->InternMaterial(input->caret_material) }, false),
                PositionProxy16{}, ParentComponent{ entry.entity }, LocalMatrixProxy16{},
                UIComponent{}, GeneratedComponent{}, TextureStateComponent{}, UITextComponent{ {}, input->text.font });
    }
    else if (entry.has_entity) {
        if (renders && object_manager->Has<Positions>(scene, entry.entity)) {
            WritePositions(object_manager, scene, entry.entity, positions);
            ctx->HideEntity(scene_name, entry.entity, entry.shown);
            if (text && object_manager->Has<UITextComponent>(scene, entry.entity))
                object_manager->GetComponent<UITextComponent>(scene, entry.entity) = MakeTextComponent(*text, width, height);
        }
        WriteClip(object_manager, scene, entry.entity, drawn_clip, clip_rect);
        if (renders && input) WriteClip(object_manager, scene, input->caret_entity, drawn_clip, std::nullopt);
    }

    for (UINodeIndex child : entry.children)
        EmitNode(ctx, object_manager, scene, scene_name, child, left - scroll.x, top - scroll.y, entry.shown, screen_width,
                 screen_height, children_clip, order, clip_count, create_entities);
}

void UI_Manager::Emit(EngineContext* ctx, float screen_width, float screen_height)
{
    if (!ctx) return;

    ObjectManager* object_manager = ctx->GetObjectManager();
    SceneData*     scene          = object_manager->GetActiveScene();
    // Без сцены выходим до гашения emit_needed и записи размера окна: иначе сцена, появившаяся позже,
    // осталась бы без UI до следующего структурного изменения.
    if (!scene) return;
    const SceneName scene_name = object_manager->GetActiveSceneName();

    const bool screen_changed = screen_width != last_screen_width || screen_height != last_screen_height;
    if (!emit_needed && !screen_changed) return;
    last_screen_width  = screen_width;
    last_screen_height = screen_height;
    emit_needed        = false;

    if (!root || screen_width <= 0.0f || screen_height <= 0.0f) {
        DeleteCreatedEntities(ctx, scene_name);
        recreate_needed = false;
        return;
    }

    YGNodeCalculateLayout(nodes[root.value].yoga_node, screen_width, screen_height, YGDirectionLTR);

    // Пересоздание — только на структурной правке: каждый кадр оно было бы структурной мутацией ECS под
    // обходом редактора с UI-потока.
    const bool create_entities = recreate_needed;
    if (create_entities) {
        DeleteCreatedEntities(ctx, scene_name);
        for (UINode& entry : nodes) entry.has_entity = false;
        recreate_needed = false;
    }
    int      order      = 0;
    uint32_t clip_count = 0;
    EmitNode(ctx, object_manager, scene, scene_name, root, 0.0f, 0.0f, true, screen_width, screen_height, std::nullopt, order,
             clip_count, create_entities);
}
