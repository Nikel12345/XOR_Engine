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

    float grow = 0.0f;
    if (style.width_mode == UISize::Grow)                         grow = style.width > 0.0f ? style.width : 1.0f;
    if (style.height_mode == UISize::Grow && style.height > 0.0f) grow = style.height;
    if (style.grow > 0.0f)                                        grow = style.grow;
    YGNodeStyleSetFlexGrow(yoga_node, grow);
    ApplyEdges(yoga_node, YGNodeStyleSetPadding, style.padding);
    ApplyEdges(yoga_node, YGNodeStyleSetMargin,  style.margin);
    YGNodeStyleSetGap(yoga_node, YGGutterAll, style.gap);
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
static WrappedSize WrapText(const UITextShape& shape, float max_width, std::vector<uint32_t>* wrapped)
{
    const std::vector<UITextShape::Glyph>& glyphs = shape.glyphs;
    WrappedSize size;
    float line_width    = 0.0f;
    bool  line_has_word = false;
    size_t i = 0;
    while (i < glyphs.size()) {
        if (glyphs[i].code == UI_LINE_BREAK_CODE) {
            size.width    = std::max(size.width, line_width);
            line_width    = 0.0f;
            line_has_word = false;
            ++size.lines;
            if (wrapped) wrapped->push_back(UI_LINE_BREAK_CODE);
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
        if (word_begin == i) continue;

        if (line_has_word && line_width + spaces_width + word_width > max_width) {
            size.width = std::max(size.width, line_width);
            line_width = word_width;
            ++size.lines;
            if (wrapped) wrapped->push_back(UI_LINE_BREAK_CODE);
        } else {
            line_width += spaces_width + word_width;
            if (wrapped) for (size_t k = spaces_begin; k < word_begin; ++k) wrapped->push_back(glyphs[k].code);
        }
        if (wrapped) for (size_t k = word_begin; k < i; ++k) wrapped->push_back(glyphs[k].code);
        line_has_word = true;
    }
    size.width = std::max(size.width, line_width);
    return size;
}

static YGSize MeasureText(YGNodeConstRef yoga_node, float width, YGMeasureMode width_mode, float, YGMeasureMode)
{
    const UITextShape& shape = *static_cast<const UITextShape*>(YGNodeGetContext(yoga_node));
    const float max_width = width_mode == YGMeasureModeUndefined ? std::numeric_limits<float>::infinity() : width;
    const WrappedSize size = WrapText(shape, max_width, nullptr);
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

static UITextComponent MakeTextComponent(const UINode& entry, float width, float height)
{
    const float line_height = entry.shape->line_height;
    return UITextComponent{ entry.wrapped_glyphs, entry.font, width / line_height, height / line_height };
}

UI_Manager::UI_Manager() : yoga_config(YGConfigNew()) {}

UI_Manager::~UI_Manager()
{
    Clear();
    if (yoga_config) YGConfigFree(yoga_config);
}

UI_Manager::Node UI_Manager::RootNode() const { return root; }

uint32_t UI_Manager::ChildCount(Node node) const
{
    return node < nodes.size() ? safe_u32(nodes[node].children.size()) : 0u;
}

UI_Manager::Node UI_Manager::ChildAt(Node node, uint32_t index) const
{
    if (node >= nodes.size() || index >= nodes[node].children.size()) return kInvalid;
    return nodes[node].children[index];
}

std::string UI_Manager::NodeLabel(Node node) const
{
    if (node >= nodes.size()) return "<invalid>";
    const UINode& entry = nodes[node];
    std::string label = node == root ? "ui" : entry.shape ? "text" : "box";
    if (!entry.id.empty()) label += "#" + entry.id;
    for (const std::string& name : entry.classes) label += "." + name;
    for (uint32_t index : entry.instance) label += "[" + std::to_string(index) + "]";
    return label;
}

UI_Manager::Node UI_Manager::FindNode(std::string_view id, const UIVarKey& instance) const
{
    if (id.empty()) return kInvalid;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id && nodes[i].instance == instance) return safe_u32(i);
    return kInvalid;
}

bool UI_Manager::IsNodeShown(Node node) const
{
    return node < nodes.size() && nodes[node].emitted && nodes[node].shown;
}

bool UI_Manager::NodeContains(Node node, float window_x, float window_y) const
{
    if (!IsNodeShown(node)) return false;
    const UINode& entry = nodes[node];
    const glm::mat2 local(entry.local);
    if (glm::determinant(local) == 0.0f) return false;

    const float center_x = (entry.ndc_center_x + 1.0f) * 0.5f * last_screen_width;
    const float center_y = (1.0f - entry.ndc_center_y) * 0.5f * last_screen_height;
    // local работает в пикселях с осью y вверх (как NDC), а окно считает y вниз.
    const glm::vec2 point = glm::inverse(local) * glm::vec2(window_x - center_x, center_y - window_y);
    return std::abs(point.x) <= 0.5f * entry.layout_width && std::abs(point.y) <= 0.5f * entry.layout_height;
}

void UI_Manager::NudgeNode(Node node, float delta_x, float delta_y, float delta_z)
{
    if (node >= nodes.size()) return;
    UINode& entry = nodes[node];
    entry.offset_x += delta_x;
    entry.offset_y += delta_y;
    entry.offset_z += delta_z;
    emit_needed = true;
}

bool UI_Manager::GetNodeNdc(Node node, float& ndc_x, float& ndc_y, float& ndc_z) const
{
    if (!IsNodeShown(node)) return false;
    const UINode& entry = nodes[node];
    ndc_x = entry.ndc_center_x;
    ndc_y = entry.ndc_center_y;
    ndc_z = entry.ndc_z;
    return true;
}

void UI_Manager::GetOffset(Node node, float& offset_x, float& offset_y, float& offset_z) const
{
    if (node >= nodes.size()) { offset_x = offset_y = offset_z = 0.0f; return; }
    const UINode& entry = nodes[node];
    offset_x = entry.offset_x;
    offset_y = entry.offset_y;
    offset_z = entry.offset_z;
}

void UI_Manager::SetNodeTransform(Node node, const float matrix[16])
{
    if (node >= nodes.size()) return;
    nodes[node].local = glm::make_mat4(matrix);
    emit_needed = true;
}

bool UI_Manager::GetNodeTransform(Node node, float matrix[16]) const
{
    if (node >= nodes.size()) return false;
    std::memcpy(matrix, glm::value_ptr(nodes[node].local), 16 * sizeof(float));
    return true;
}

void UI_Manager::SetNodeStyle(Node node, const UIStyle& style)
{
    if (node >= nodes.size()) return;
    UINode& entry = nodes[node];
    entry.style = style;
    if (entry.shape) entry.style.padding = {};
    ApplyStyle(entry.yoga_node, entry.style, entry.parked);
    emit_needed = true;
}

void UI_Manager::SetNodeParked(Node node, bool parked)
{
    UINode& entry = nodes[node];
    if (entry.parked == parked) return;
    entry.parked = parked;
    ApplyStyle(entry.yoga_node, entry.style, parked);
    emit_needed = true;
}

void UI_Manager::RebuildChildren(Node parent)
{
    std::vector<Node> order;
    AppendSlotNodes(nodes[parent].slots, order);
    std::vector<YGNodeRef> yoga_children;
    yoga_children.reserve(order.size());
    for (Node child : order) yoga_children.push_back(nodes[child].yoga_node);
    YGNodeSetChildren(nodes[parent].yoga_node, yoga_children.data(), yoga_children.size());
    nodes[parent].children = std::move(order);
    emit_needed = true;
}

bool UI_Manager::GetNodeStyle(Node node, UIStyle& style) const
{
    if (node >= nodes.size()) return false;
    style = nodes[node].style;
    return true;
}

bool UI_Manager::GetNodeLayoutSize(Node node, float& width, float& height) const
{
    if (node >= nodes.size() || !nodes[node].emitted) return false;
    width  = nodes[node].layout_width;
    height = nodes[node].layout_height;
    return true;
}

bool UI_Manager::IsTextNode(Node node) const
{
    return node < nodes.size() && nodes[node].shape != nullptr;
}

void UI_Manager::SetText(Node node, const std::string& utf8, const FontManager* font_manager)
{
    if (node >= nodes.size() || !font_manager) return;
    UINode& entry = nodes[node];
    const FontData* font = font_manager->GetFont(entry.font);
    if (!entry.shape || !font) return;

    std::vector<UITextShape::Glyph> glyphs = ShapeGlyphs(font, font_manager, utf8);
    if (glyphs == entry.shape->glyphs) return;
    entry.shape->glyphs = std::move(glyphs);
    // Yoga кэширует замер узла с measure-функцией и без пометки не спросит размер заново.
    YGNodeMarkDirty(entry.yoga_node);
    emit_needed = true;
}

void UI_Manager::SetNodeMaterial(EngineContext* ctx, Node node, const std::string& material)
{
    if (node >= nodes.size()) return;
    UINode& entry = nodes[node];
    if (entry.material == material || (entry.shape && material.empty())) return;
    entry.material = material;

    const bool draws = entry.shape || !material.empty();
    if (draws != entry.draws) {
        entry.draws     = draws;
        emit_needed     = true;
        recreate_needed = true;
    }
    else if (entry.has_entity) {
        ctx->ChangeMaterial(entry.entity, material);
    }
}

bool UI_Manager::GetNodeEntity(Node node, Entity& entity) const
{
    if (node >= nodes.size() || !nodes[node].has_entity) return false;
    entity = nodes[node].entity;
    return true;
}

UI_Manager::Node UI_Manager::MakeNode(const UIStyle& style)
{
    YGNodeRef yoga_node = YGNodeNewWithConfig(yoga_config);
    ApplyStyle(yoga_node, style, false);
    const Node node = safe_u32(nodes.size());
    nodes.push_back(UINode{ yoga_node, style });   // может реаллоцировать: ссылки на узлы — только после него
    return node;
}

void UI_Manager::AttachChild(Node parent, Node child)
{
    UINode& parent_entry = nodes[parent];
    YGNodeInsertChild(parent_entry.yoga_node, nodes[child].yoga_node, YGNodeGetChildCount(parent_entry.yoga_node));
    parent_entry.children.push_back(child);
    emit_needed     = true;
    recreate_needed = true;
}

UI_Manager::Node UI_Manager::Root(const UIStyle& style)
{
    Clear();
    root = MakeNode(style);
    return root;
}

UI_Manager::Node UI_Manager::Box(Node parent, const UIStyle& style, const std::string& material, const std::string& quad)
{
    const Node node = MakeNode(style);
    UINode& entry = nodes[node];
    entry.material = material;
    entry.quad     = quad;
    entry.draws    = !material.empty();
    AttachChild(parent, node);
    return node;
}

UI_Manager::Node UI_Manager::Text(Node parent, const UIStyle& style, const std::string& utf8,
                                  const std::string& material, const std::string& quad, FontId font,
                                  const FontManager* font_manager)
{
    // ui.frag рисует текст от угла рамки узла и об отступе внутри неё не знает.
    UIStyle text_style = style;
    if (text_style.padding != UIEdges{}) {
        SDL_Log("UI_Manager::Text: padding belongs to the parent box, ignored");
        text_style.padding = {};
    }
    const Node node = MakeNode(text_style);
    UINode& entry = nodes[node];
    entry.material = material;
    entry.quad     = quad;
    entry.draws    = true;
    entry.font     = font;
    if (const FontData* font_data = font_manager ? font_manager->GetFont(font) : nullptr) {
        entry.shape = std::make_unique<UITextShape>();
        entry.shape->line_height = safe_sint32_f(font_data->line_height);
        entry.shape->glyphs      = ShapeGlyphs(font_data, font_manager, utf8);
        // Адрес shape — контекст measure-функции, поэтому shape живёт отдельной аллокацией: nodes
        // переезжает при росте.
        YGNodeSetContext(entry.yoga_node, entry.shape.get());
        YGNodeSetMeasureFunc(entry.yoga_node, MeasureText);
    }
    AttachChild(parent, node);
    return node;
}

void UI_Manager::Clear()
{
    // Box и Text всегда вставляют узел в родителя, поэтому free корня освобождает всё дерево.
    if (root != kInvalid && nodes[root].yoga_node) YGNodeFreeRecursive(nodes[root].yoga_node);
    nodes.clear();
    root            = kInvalid;
    emit_needed     = true;
    recreate_needed = true;

    elements.clear();
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
    created_entities.clear();   // энтити не удаляем: их уже нет, id принадлежат новой сцене
}

void UI_Manager::DeleteCreatedEntities(EngineContext* ctx, const SceneName& scene_name)
{
    for (Entity entity : created_entities) ctx->DeleteEntity(scene_name, entity);
    created_entities.clear();
}

void UI_Manager::EmitNode(EngineContext* ctx, ObjectManager* object_manager, SceneData* scene, const SceneName& scene_name,
                          Node node, float parent_x, float parent_y, bool parent_shown, float screen_width, float screen_height,
                          int& order, bool create_entities)
{
    constexpr float kZStep = 0.00001f;

    UINode& entry = nodes[node];
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
    entry.layout_width  = width;
    entry.layout_height = height;
    entry.emitted       = true;
    entry.shown         = parent_shown && entry.style.display != UIDisplay::None && !entry.parked;

    if (entry.shape) {
        // Yoga округляет раскладку до пикселя, и ширина узла расходится с той, по которой мерил
        // MeasureText, на долю пикселя: без запаса строка, влезшая при замере, здесь перенеслась бы.
        constexpr float kWrapSlack = 1.0f;
        entry.wrapped_glyphs.clear();
        WrapText(*entry.shape, width + kWrapSlack, &entry.wrapped_glyphs);
    }

    if (entry.draws && !entry.quad.empty()) {
        PositionProxy16 positions{ transform[0][0], transform[0][1], transform[0][2], transform[3][0],
                                   transform[1][0], transform[1][1], transform[1][2], transform[3][1],
                                   transform[2][0], transform[2][1], transform[2][2], transform[3][2],
                                   transform[0][3], transform[1][3], transform[2][3], transform[3][3] };

        if (create_entities) {
            // GeneratedComponent: узлы пересоздаёт Emit, их копия в scene.sheaf дала бы дубли при загрузке.
            // TextureStateComponent ставится всегда: тег вешается только при создании, а материал узлу
            // могут сменить на вариативный позже.
            if (entry.shape)
                entry.entity = ctx->CreateEntity(scene_name,
                    RenderableProxy::Single(ctx->GetModelManager()->InternModel(entry.quad),
                                            { ctx->GetMaterialManager()->InternMaterial(entry.material) }, entry.shown),
                    positions, UIComponent{}, GeneratedComponent{}, TextureStateComponent{},
                    MakeTextComponent(entry, width, height));
            else
                entry.entity = ctx->CreateEntity(scene_name,
                    RenderableProxy::Single(ctx->GetModelManager()->InternModel(entry.quad),
                                            { ctx->GetMaterialManager()->InternMaterial(entry.material) }, entry.shown),
                    positions, UIComponent{}, GeneratedComponent{}, TextureStateComponent{});
            entry.has_entity = true;
            created_entities.push_back(entry.entity);
        }
        else if (entry.has_entity && object_manager->Has<Positions>(scene, entry.entity)) {
            WritePositions(object_manager, scene, entry.entity, positions);
            ctx->HideEntity(scene_name, entry.entity, entry.shown);
            if (entry.shape && object_manager->Has<UITextComponent>(scene, entry.entity))
                object_manager->GetComponent<UITextComponent>(scene, entry.entity) = MakeTextComponent(entry, width, height);
        }
    }

    for (Node child : entry.children)
        EmitNode(ctx, object_manager, scene, scene_name, child, left, top, entry.shown, screen_width, screen_height, order,
                 create_entities);
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

    if (root == kInvalid || screen_width <= 0.0f || screen_height <= 0.0f) {
        DeleteCreatedEntities(ctx, scene_name);
        recreate_needed = false;
        return;
    }

    YGNodeCalculateLayout(nodes[root].yoga_node, screen_width, screen_height, YGDirectionLTR);

    // Пересоздание — только на структурной правке: каждый кадр оно было бы структурной мутацией ECS под
    // обходом редактора с UI-потока.
    const bool create_entities = recreate_needed;
    if (create_entities) {
        DeleteCreatedEntities(ctx, scene_name);
        for (UINode& entry : nodes) entry.has_entity = false;
        recreate_needed = false;
    }
    int order = 0;
    EmitNode(ctx, object_manager, scene, scene_name, root, 0.0f, 0.0f, true, screen_width, screen_height, order, create_entities);
}
