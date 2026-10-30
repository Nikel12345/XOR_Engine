#include "PCH.h"
#include "UI_Manager.h"
#include "EngineContext.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "FontManager.h"
#include <yoga/Yoga.h>

static void ApplyEdges(YGNodeRef n, void (*set)(YGNodeRef, YGEdge, float), const UIEdges& e)
{
    set(n, YGEdgeTop,    e.top);
    set(n, YGEdgeRight,  e.right);
    set(n, YGEdgeBottom, e.bottom);
    set(n, YGEdgeLeft,   e.left);
}

static void ApplyStyle(YGNodeRef n, const UIStyle& s)
{
    YGNodeStyleSetFlexDirection(n, s.dir == UIDir::Row ? YGFlexDirectionRow : YGFlexDirectionColumn);

    switch (s.justify) {
        case UIJustify::Start:        YGNodeStyleSetJustifyContent(n, YGJustifyFlexStart);    break;
        case UIJustify::Center:       YGNodeStyleSetJustifyContent(n, YGJustifyCenter);       break;
        case UIJustify::End:          YGNodeStyleSetJustifyContent(n, YGJustifyFlexEnd);      break;
        case UIJustify::SpaceBetween: YGNodeStyleSetJustifyContent(n, YGJustifySpaceBetween); break;
    }
    switch (s.align) {
        case UIAlign::Start:   YGNodeStyleSetAlignItems(n, YGAlignFlexStart); break;
        case UIAlign::Center:  YGNodeStyleSetAlignItems(n, YGAlignCenter);    break;
        case UIAlign::End:     YGNodeStyleSetAlignItems(n, YGAlignFlexEnd);   break;
        case UIAlign::Stretch: YGNodeStyleSetAlignItems(n, YGAlignStretch);   break;
    }

    // Каждое поле пишется явно, и умолчание тоже: стиль применяется повторно к живому узлу
    // (SetNodeStyle), и поле, сброшенное в Auto или 0, обязано сброситься и в Yoga.
    switch (s.wmode) {
        case UISize::Points:  YGNodeStyleSetWidth(n, s.w);        break;
        case UISize::Percent: YGNodeStyleSetWidthPercent(n, s.w); break;
        default:              YGNodeStyleSetWidthAuto(n);         break;
    }
    switch (s.hmode) {
        case UISize::Points:  YGNodeStyleSetHeight(n, s.h);        break;
        case UISize::Percent: YGNodeStyleSetHeightPercent(n, s.h); break;
        default:              YGNodeStyleSetHeightAuto(n);         break;
    }

    float grow = 0.0f;
    if (s.wmode == UISize::Grow)            grow = s.w > 0.0f ? s.w : 1.0f;
    if (s.hmode == UISize::Grow && s.h > 0) grow = s.h;
    if (s.grow > 0.0f)                      grow = s.grow;
    YGNodeStyleSetFlexGrow(n, grow);
    ApplyEdges(n, YGNodeStyleSetPadding, s.padding);
    ApplyEdges(n, YGNodeStyleSetMargin,  s.margin);
    YGNodeStyleSetGap(n, YGGutterAll, s.gap);
}

static std::vector<UITextShape::Glyph> ShapeGlyphs(const FontData* font, const FontManager* fm, const std::string& utf8)
{
    std::vector<UITextShape::Glyph> glyphs;
    for (uint32_t code : fm->ShapeString(font, utf8)) {
        const GlyphInfo* glyph = code < font->glyphs.size() ? &font->glyphs[code] : nullptr;
        glyphs.push_back({ code, glyph ? safe_sint32_f(glyph->advance) : 0.0f,
                           glyph && glyph->codepoint == ' ' });
    }
    return glyphs;
}

struct WrappedSize {
    float width = 0.0f;
    int   lines = 1;
};

// Жадный перенос по пробелам. Пробелы на месте автопереноса и в конце строки выпадают; слово шире
// max_width не рвётся и вылезает за узел, как в CSS. out == nullptr — только замер.
static WrappedSize WrapText(const UITextShape& shape, float max_width, std::vector<uint32_t>* out)
{
    const std::vector<UITextShape::Glyph>& glyphs = shape.glyphs;
    WrappedSize size;
    float line_width    = 0.0f;
    bool  line_has_word = false;
    size_t i = 0;
    while (i < glyphs.size()) {
        if (glyphs[i].code == UI_LINE_BREAK_CODE) {
            size.width = std::max(size.width, line_width);
            line_width = 0.0f;  line_has_word = false;  ++size.lines;
            if (out) out->push_back(UI_LINE_BREAK_CODE);
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
            line_width = word_width;  ++size.lines;
            if (out) out->push_back(UI_LINE_BREAK_CODE);
        } else {
            line_width += spaces_width + word_width;
            if (out) for (size_t k = spaces_begin; k < word_begin; ++k) out->push_back(glyphs[k].code);
        }
        if (out) for (size_t k = word_begin; k < i; ++k) out->push_back(glyphs[k].code);
        line_has_word = true;
    }
    size.width = std::max(size.width, line_width);
    return size;
}

static YGSize MeasureText(YGNodeConstRef node, float width, YGMeasureMode width_mode, float, YGMeasureMode)
{
    const UITextShape& shape = *static_cast<const UITextShape*>(YGNodeGetContext(node));
    const float max_width = (width_mode == YGMeasureModeUndefined) ? std::numeric_limits<float>::infinity() : width;
    const WrappedSize size = WrapText(shape, max_width, nullptr);
    return { size.width, safe_sint32_f(size.lines) * shape.line_height };
}

UI_Manager::UI_Manager() : cfg(YGConfigNew()) {}

UI_Manager::~UI_Manager()
{
    Clear();
    if (cfg) YGConfigFree(cfg);
}

bool UI_Manager::HasTree() const { return root != kInvalid; }

UI_Manager::Node UI_Manager::RootNode() const { return root; }

uint32_t UI_Manager::ChildCount(Node n) const
{
    return (n < nodes.size()) ? static_cast<uint32_t>(nodes[n].children.size()) : 0u;
}

UI_Manager::Node UI_Manager::ChildAt(Node n, uint32_t i) const
{
    if (n >= nodes.size() || i >= nodes[n].children.size()) return kInvalid;
    return nodes[n].children[i];
}

std::string UI_Manager::NodeLabel(Node n) const
{
    if (n >= nodes.size()) return "<invalid>";
    const UINode& r = nodes[n];
    const std::string id = r.id.empty() ? std::string() : " #" + r.id;
    if (n == root)           return "UI Root" + id;
    if (!r.text.empty())     return "Text" + id + ": " + r.text;
    if (!r.material.empty()) return "Panel" + id;
    return "Group" + id;
}

void UI_Manager::NudgeNode(Node n, float ddx, float ddy, float ddz)
{
    if (n >= nodes.size()) return;
    UINode& r = nodes[n];
    r.off_x += ddx;  r.off_y += ddy;  r.z_off += ddz;
    dirty = true;
}

bool UI_Manager::GetNodeNdc(Node n, float& ndc_x, float& ndc_y, float& z) const
{
    if (n >= nodes.size() || !nodes[n].emitted) return false;
    const UINode& r = nodes[n];
    ndc_x = r.ndc_cx;  ndc_y = r.ndc_cy;  z = r.ndc_z;
    return true;
}

void UI_Manager::GetOffset(Node n, float& dx, float& dy, float& dz) const
{
    if (n >= nodes.size()) { dx = dy = dz = 0.0f; return; }
    const UINode& r = nodes[n];
    dx = r.off_x;  dy = r.off_y;  dz = r.z_off;
}

void UI_Manager::SetNodeTransform(Node n, const float m[16])
{
    if (n >= nodes.size()) return;
    nodes[n].local = glm::make_mat4(m);
    dirty = true;
}

bool UI_Manager::GetNodeTransform(Node n, float m[16]) const
{
    if (n >= nodes.size()) return false;
    std::memcpy(m, glm::value_ptr(nodes[n].local), 16 * sizeof(float));
    return true;
}

void UI_Manager::SetNodeStyle(Node n, const UIStyle& s)
{
    if (n >= nodes.size()) return;
    UINode& r = nodes[n];
    r.style = s;
    if (r.shape) r.style.padding = {};   // см. UI_Manager::Text
    ApplyStyle(r.yg, r.style);
    dirty = true;
}

bool UI_Manager::GetNodeStyle(Node n, UIStyle& s) const
{
    if (n >= nodes.size()) return false;
    s = nodes[n].style;
    return true;
}

bool UI_Manager::GetNodeLayoutSize(Node n, float& w, float& h) const
{
    if (n >= nodes.size() || !nodes[n].emitted) return false;
    w = nodes[n].layout_w;  h = nodes[n].layout_h;
    return true;
}

bool UI_Manager::IsTextNode(Node n) const
{
    return n < nodes.size() && nodes[n].shape != nullptr;
}

void UI_Manager::SetText(Node n, const std::string& utf8, const FontManager* fm)
{
    if (n >= nodes.size() || !fm) return;
    UINode& r = nodes[n];
    const FontData* font = fm->GetFont(r.font);
    if (!r.shape || !font) return;

    std::vector<UITextShape::Glyph> glyphs = ShapeGlyphs(font, fm, utf8);
    if (glyphs == r.shape->glyphs) return;
    r.shape->glyphs = std::move(glyphs);
    // Yoga кэширует замер узла с measure-функцией и без пометки не спросит размер заново.
    YGNodeMarkDirty(r.yg);
    dirty = true;
}

void UI_Manager::SetNodeMaterial(EngineContext* ctx, Node n, const std::string& material)
{
    if (n >= nodes.size()) return;
    UINode& r = nodes[n];
    if (r.material == material || (r.shape && material.empty())) return;
    r.material = material;

    const bool draws = r.shape || !material.empty();
    if (draws != r.draws) {
        r.draws = draws;
        dirty = true;  structural = true;
    }
    else if (r.has_entity) {
        ctx->ChangeMaterial(r.entity, material);
    }
}

bool UI_Manager::GetNodeEntity(Node n, uint32_t& entity) const
{
    if (n >= nodes.size() || !nodes[n].has_entity) return false;
    entity = nodes[n].entity;
    return true;
}

UI_Manager::Node UI_Manager::MakeNode(const UIStyle& s)
{
    YGNodeRef yg = YGNodeNewWithConfig(cfg);
    ApplyStyle(yg, s);
    const Node id = static_cast<Node>(nodes.size());
    nodes.push_back(UINode{ yg, s });   // push_back МОЖЕТ реаллоцировать — берём ссылки ПОСЛЕ
    return id;
}

UI_Manager::Node UI_Manager::Root(const UIStyle& s)
{
    Clear();
    const Node id = MakeNode(s);
    root = id;
    dirty = true;  structural = true;
    return id;
}

UI_Manager::Node UI_Manager::Box(Node parent, const UIStyle& s, const std::string& material, const std::string& quad)
{
    const Node id = MakeNode(s);   // единственный push_back — ссылки берём после него
    UINode& r = nodes[id];
    r.material = material;
    r.quad     = quad;
    r.draws    = !material.empty();
    UINode& p = nodes[parent];
    YGNodeInsertChild(p.yg, r.yg, YGNodeGetChildCount(p.yg));
    p.children.push_back(id);
    dirty = true;  structural = true;
    return id;
}

UI_Manager::Node UI_Manager::Text(Node parent, const UIStyle& s, const std::string& utf8,
                                  const std::string& material, const std::string& quad, FontId font, const FontManager* fm)
{
    // ui.frag рисует текст от угла рамки узла и об отступе внутри неё не знает.
    UIStyle text_style = s;
    if (text_style.padding != UIEdges{}) {
        SDL_Log("UI_Manager::Text: padding belongs to the parent box, ignored");
        text_style.padding = {};
    }
    const Node id = MakeNode(text_style);
    UINode& r = nodes[id];
    r.material = material;
    r.quad     = quad;
    r.draws    = true;
    r.font     = font;
    r.text     = utf8;
    if (const FontData* font_data = fm ? fm->GetFont(font) : nullptr) {
        r.shape = std::make_unique<UITextShape>();
        r.shape->line_height = safe_sint32_f(font_data->line_height);
        r.shape->glyphs = ShapeGlyphs(font_data, fm, utf8);
        YGNodeSetContext(r.yg, r.shape.get());
        YGNodeSetMeasureFunc(r.yg, MeasureText);
    }
    UINode& p = nodes[parent];
    YGNodeInsertChild(p.yg, r.yg, YGNodeGetChildCount(p.yg));
    p.children.push_back(id);
    dirty = true;  structural = true;
    return id;
}

void UI_Manager::Clear()
{
    // Все узлы достижимы из корня (Box/Text всегда вставляются в родителя) → рекурсивный free
    // корня освобождает всё дерево. Созданные энтити тут НЕ трогаем (нет ctx) — они снимутся на
    // следующем Emit по created.
    if (root != kInvalid && nodes[root].yg)
        YGNodeFreeRecursive(nodes[root].yg);
    nodes.clear();
    root = kInvalid;
    dirty = true;  structural = true;
}

void UI_Manager::Reset()
{
    Clear();           // дерево — под нож (Yoga-узлы + записи), dirty/structural взводит сам
    created.clear();   // а вот энтити НЕ удаляем: их уже нет, id принадлежат новой сцене
}

// Запись матрицы узла в Positions существующей энтити (мутация НА МЕСТЕ, без recreate). row-major:
// поля PositionProxy16 ложатся в одноимённые SoA-колонки Positions.
static void WritePositions(ObjectManager* om, SceneData* scene, Entity e, const PositionProxy16& m)
{
    SoAElement<Positions> el = om->GetComponent<Positions>(scene, e);
    Positions& P = el.container();
    const size_t i = el.i();
    P.x[i]=m.x; P.y[i]=m.y; P.z[i]=m.z; P.w[i]=m.w;
    P.a[i]=m.a; P.b[i]=m.b; P.c[i]=m.c; P.d[i]=m.d;
    P.e[i]=m.e; P.f[i]=m.f; P.g[i]=m.g; P.h[i]=m.h;
    P.i[i]=m.i; P.j[i]=m.j; P.k[i]=m.k; P.l[i]=m.l;
}

static UITextComponent MakeTextComponent(const UINode& r, float w, float h)
{
    return UITextComponent{ r.glyphs, r.font, w / r.shape->line_height, h / r.shape->line_height };
}

// Рекурсивный обход: аккумулируем абсолютный px-офсет, считаем rect → NDC-матрицу узла. create=true —
// СОЗДАЁМ энтити (структурный проход); create=false — МУТИРУЕМ Positions существующей энтити на месте
// (layout-правки: драг/ресайз — без recreate, иначе структурная мутация ECS каждый кадр роняет
// редактор, см. CLAUDE.md).
void UI_Manager::EmitNode(EngineContext* ctx, ObjectManager* om, SceneData* scene_ptr,
                          const SceneName& scene_name, Node id, float ox, float oy,
                          float W, float H, int& order, bool create)
{
    constexpr float kZStep = 0.00001f;

    UINode& r = nodes[id];
    const float x = ox + YGNodeLayoutGetLeft(r.yg) + r.off_x;
    const float y = oy + YGNodeLayoutGetTop(r.yg)  + r.off_y;
    const float w = YGNodeLayoutGetWidth(r.yg);
    const float h = YGNodeLayoutGetHeight(r.yg);

    const float z = 0.5f - safe_sint32_f(order++) * kZStep + r.z_off;

    const glm::mat3 to_ndc(glm::vec3(2.0f / W, 0.0f, 0.0f),
                           glm::vec3(0.0f, 2.0f / H, 0.0f),
                           glm::vec3(0.0f, 0.0f, 2.0f / H));
    const glm::mat3 size(glm::vec3(w, 0.0f, 0.0f), glm::vec3(0.0f, h, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat3 basis = to_ndc * glm::mat3(r.local) * size;

    const float tx = ((x + 0.5f * w) / W) * 2.0f - 1.0f + r.local[3][0] * 2.0f / W;
    const float ty = 1.0f - ((y + 0.5f * h) / H) * 2.0f + r.local[3][1] * 2.0f / H;

    glm::mat4 M(basis);
    M[3] = glm::vec4(tx, ty, z, 1.0f);
    r.ndc_cx = tx;
    r.ndc_cy = ty;
    r.ndc_z  = z;
    r.layout_w = w;
    r.layout_h = h;
    r.emitted = true;

    if (r.shape) {
        // Yoga округляет раскладку до пикселя, и ширина узла расходится с той, по которой мерил
        // MeasureText, на долю пикселя: без запаса строка, влезшая при замере, здесь перенеслась бы.
        constexpr float kWrapSlack = 1.0f;
        r.glyphs.clear();
        WrapText(*r.shape, w + kWrapSlack, &r.glyphs);
    }

    if (r.draws && !r.quad.empty()) {
        PositionProxy16 m{ M[0][0], M[0][1], M[0][2], M[3][0],
                           M[1][0], M[1][1], M[1][2], M[3][1],
                           M[2][0], M[2][1], M[2][2], M[3][2],
                           M[0][3], M[1][3], M[2][3], M[3][3] };

        if (create) {
            Entity e;
            // GeneratedComponent → SaveScene не пишет их в scene.sheaf: узлы UI пересоздаёт
            // UI_Manager::Emit из дерева каждую сессию, копия в файле = дубли/баги при загрузке.
            // TextureStateComponent — БЕЗУСЛОВНО: тег ставится только при создании (миграции
            // архетипов нет), а материал узлу можно сменить на вариативный позже. Узлов UI
            // десятки, лишний элемент из нулей в буфере состояний ничего не стоит.
            if (r.shape)
                e = ctx->CreateEntity(scene_name,
                        RenderableProxy::Single(ctx->GetModelManager()->InternModel(r.quad),
                                                { ctx->GetMaterialManager()->InternMaterial(r.material) }),
                        m, UIComponent{}, GeneratedComponent{}, TextureStateComponent{},
                        MakeTextComponent(r, w, h));
            else
                e = ctx->CreateEntity(scene_name,
                        RenderableProxy::Single(ctx->GetModelManager()->InternModel(r.quad),
                                                { ctx->GetMaterialManager()->InternMaterial(r.material) }),
                        m, UIComponent{}, GeneratedComponent{}, TextureStateComponent{});
            r.entity = e;  r.has_entity = true;
            created.push_back(e);
        }
        else if (r.has_entity && om->Has<Positions>(scene_ptr, r.entity)) {
            WritePositions(om, scene_ptr, r.entity, m);   // мутация на месте (без recreate)
            if (r.shape && om->Has<UITextComponent>(scene_ptr, r.entity))
                om->GetComponent<UITextComponent>(scene_ptr, r.entity) = MakeTextComponent(r, w, h);
        }
    }

    for (Node ch : r.children)
        EmitNode(ctx, om, scene_ptr, scene_name, ch, x, y, W, H, order, create);
}

void UI_Manager::Emit(EngineContext* ctx, float screen_w, float screen_h)
{
    if (!ctx) return;

    ObjectManager* om       = ctx->GetObjectManager();
    SceneData*     scene    = om->GetActiveScene();
    // Узлы UI — обычные энтити активной сцены, класть их некуда. Выходим ДО гашения dirty и до
    // записи last_w/h: иначе состояние «раскладка не выложена» считалось бы обработанным, и
    // появившаяся позже сцена осталась бы без UI до следующего структурного изменения.
    if (!scene) return;
    const SceneName scene_name = om->GetActiveSceneName();

    // Смена размера экрана инвалидирует раскладку так же, как dirty (см. last_w/h): без этого
    // ресайз не перекладывал UI — узлы держали NDC от стартового разрешения, отсюда «другой размер UI».
    const bool size_changed = (screen_w != last_w) || (screen_h != last_h);
    if (!dirty && !size_changed) return;
    last_w = screen_w;  last_h = screen_h;

    if (root == kInvalid || screen_w <= 0.0f || screen_h <= 0.0f) {
        for (Entity e : created) ctx->DeleteEntity(scene_name, e);
        created.clear();
        structural = false;  dirty = false;  return;
    }

    YGNodeCalculateLayout(nodes[root].yg, screen_w, screen_h, YGDirectionLTR);

    int order = 0;
    if (structural) {
        // Структурное изменение дерева (сборка/добавление узлов, редко) → полный recreate.
        for (Entity e : created) ctx->DeleteEntity(scene_name, e);
        created.clear();
        for (UINode& n : nodes) { n.has_entity = false; n.entity = static_cast<Entity>(-1); }
        EmitNode(ctx, om, scene, scene_name, root, 0.0f, 0.0f, screen_w, screen_h, order, /*create*/true);
        structural = false;
    }
    else {
        // Только раскладка/смещение (драг гизмо, ресайз) → мутируем Positions существующих энтити на
        // месте, БЕЗ create/delete: иначе структурная мутация ECS каждый кадр против ForEach редактора
        // = краш (см. CLAUDE.md). Бонус: ресайз больше не тасует порядок батчей.
        EmitNode(ctx, om, scene, scene_name, root, 0.0f, 0.0f, screen_w, screen_h, order, /*create*/false);
    }
    dirty = false;
}
