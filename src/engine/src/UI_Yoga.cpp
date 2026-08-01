#include "PCH.h"
#include "UI_Yoga.h"
#include "EngineContext.h"    // CreateEntity/DeleteEntity (+ ObjectManager.h транзитивно)
#include "ObjectManager.h"    // GetActiveSceneName
#include "BaseComponents.h"   // Draw/Model/Material/UI/UIText компоненты + PositionProxy16 + Entity
#include "MaterialData.h"     // Material (по указателю в MaterialComponent)
#include "ModelData.h"        // ModelData
#include "FontManager.h"      // FontData/FontManager (ShapeString + метрики для intrinsic-размера)
#include <yoga/Yoga.h>

// Узел нашего дерева: Yoga-узел + пейлоад для создания энтити + свои дети (индексы, параллельны
// детям Yoga — вставляем в том же порядке). Плоский вектор в Impl, Node = индекс в нём.
struct NodeRec {
    YGNodeRef             yg       = nullptr;
    bool                  draws    = false;      // создавать ли энтити (контейнер без материала — нет)
    Material*             material = nullptr;
    ModelData*            quad     = nullptr;
    std::vector<uint32_t> glyphs;                // непусто → текстовый бокс (UITextComponent)
    uint32_t              font     = 0;
    std::string           text;                  // исходная строка (для подписи в редакторе)
    std::vector<uint32_t> children;              // индексы в Impl::nodes
    // Правка из редактора: смещение поверх посчитанной раскладки (offset px XY + z-bias слоя).
    float                 off_x = 0.0f, off_y = 0.0f, z_off = 0.0f;
    // Слепок последнего Emit (центр в NDC + z) — для постановки гизмо. Пишет sim, читает UI (benign).
    float                 ndc_cx = 0.0f, ndc_cy = 0.0f, ndc_z = 0.5f;
    bool                  emitted = false;
    // Энтити этого узла (если рисуется) — для мутации Positions на месте на layout-правках (без recreate).
    Entity                entity = static_cast<Entity>(-1);
    bool                  has_entity = false;
};

struct UI_Yoga::Impl {
    YGConfigRef          cfg  = nullptr;
    std::vector<NodeRec> nodes;
    std::vector<Entity>  created;                // энтити прошлого Emit (снимаются на пересборке)
    Node                 root = UI_Yoga::kInvalid;
    // Размер экрана прошлой раскладки. Раскладка зависит от него (points-узлы дают разную ДОЛЮ экрана
    // при разном разрешении) → его смена = layout-инвалидация, даже без dirty_. -1 = ещё не раскладывали.
    float                last_w = -1.0f, last_h = -1.0f;
};

// ── Трансляция плоского UIStyle → Yoga-сеттеры. Yoga живёт ТОЛЬКО в этом .cpp. ──
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

    switch (s.wmode) {
        case UISize::Points:  YGNodeStyleSetWidth(n, s.w);              break;
        case UISize::Percent: YGNodeStyleSetWidthPercent(n, s.w);       break;
        case UISize::Grow:    YGNodeStyleSetFlexGrow(n, s.w > 0 ? s.w : 1.0f); break;
        case UISize::Auto:    default:                                  break;   // WidthAuto = дефолт
    }
    switch (s.hmode) {
        case UISize::Points:  YGNodeStyleSetHeight(n, s.h);            break;
        case UISize::Percent: YGNodeStyleSetHeightPercent(n, s.h);     break;
        case UISize::Grow:    if (s.h > 0) YGNodeStyleSetFlexGrow(n, s.h); break;
        case UISize::Auto:    default:                                 break;
    }

    if (s.grow    > 0.0f) YGNodeStyleSetFlexGrow(n, s.grow);
    if (s.padding > 0.0f) YGNodeStyleSetPadding(n, YGEdgeAll, s.padding);
    if (s.margin  > 0.0f) YGNodeStyleSetMargin (n, YGEdgeAll, s.margin);
    if (s.gap     > 0.0f) YGNodeStyleSetGap(n, YGGutterAll, s.gap);
}

UI_Yoga::UI_Yoga() : impl_(new Impl) { impl_->cfg = YGConfigNew(); }

UI_Yoga::~UI_Yoga()
{
    Clear();
    if (impl_->cfg) YGConfigFree(impl_->cfg);
    delete impl_;
}

bool UI_Yoga::HasTree() const { return impl_->root != kInvalid; }

// ── Обход дерева для редактора ──
UI_Yoga::Node UI_Yoga::RootNode() const { return impl_->root; }

uint32_t UI_Yoga::ChildCount(Node n) const
{
    return (n < impl_->nodes.size()) ? static_cast<uint32_t>(impl_->nodes[n].children.size()) : 0u;
}

UI_Yoga::Node UI_Yoga::ChildAt(Node n, uint32_t i) const
{
    if (n >= impl_->nodes.size() || i >= impl_->nodes[n].children.size()) return kInvalid;
    return impl_->nodes[n].children[i];
}

std::string UI_Yoga::NodeLabel(Node n) const
{
    if (n >= impl_->nodes.size()) return "<invalid>";
    const NodeRec& r = impl_->nodes[n];
    if (n == impl_->root)   return "UI Root";
    if (!r.text.empty())    return "Text: " + r.text;
    if (r.material)         return "Panel";
    return "Group";
}

void UI_Yoga::NudgeNode(Node n, float ddx, float ddy, float ddz)
{
    if (n >= impl_->nodes.size()) return;
    NodeRec& r = impl_->nodes[n];
    r.off_x += ddx;  r.off_y += ddy;  r.z_off += ddz;
    dirty_ = true;   // пересчёт на следующем Emit
}

bool UI_Yoga::GetNodeNdc(Node n, float& ndc_x, float& ndc_y, float& z) const
{
    if (n >= impl_->nodes.size() || !impl_->nodes[n].emitted) return false;
    const NodeRec& r = impl_->nodes[n];
    ndc_x = r.ndc_cx;  ndc_y = r.ndc_cy;  z = r.ndc_z;
    return true;
}

void UI_Yoga::GetOffset(Node n, float& dx, float& dy, float& dz) const
{
    if (n >= impl_->nodes.size()) { dx = dy = dz = 0.0f; return; }
    const NodeRec& r = impl_->nodes[n];
    dx = r.off_x;  dy = r.off_y;  dz = r.z_off;
}

// Общий конструктор узла: создать Yoga-узел, применить стиль, положить в плоский вектор.
static UI_Yoga::Node MakeNode(UI_Yoga::Impl* impl, const UIStyle& s)
{
    YGNodeRef yg = YGNodeNewWithConfig(impl->cfg);
    ApplyStyle(yg, s);
    const UI_Yoga::Node id = static_cast<UI_Yoga::Node>(impl->nodes.size());
    impl->nodes.push_back(NodeRec{ yg });   // push_back МОЖЕТ реаллоцировать — берём ссылки ПОСЛЕ
    return id;
}

UI_Yoga::Node UI_Yoga::Root(const UIStyle& s)
{
    Clear();                        // новый корень = свежее дерево
    const Node id = MakeNode(impl_, s);
    impl_->root = id;
    dirty_ = true;  structural_ = true;
    return id;
}

UI_Yoga::Node UI_Yoga::Box(Node parent, const UIStyle& s, Material* material, ModelData* quad)
{
    const Node id = MakeNode(impl_, s);   // единственный push_back — ссылки берём после него
    NodeRec& r = impl_->nodes[id];
    r.material = material;
    r.quad     = quad;
    r.draws    = (material != nullptr);    // без материала — чистый контейнер (не рисуется)
    NodeRec& p = impl_->nodes[parent];
    YGNodeInsertChild(p.yg, r.yg, YGNodeGetChildCount(p.yg));
    p.children.push_back(id);
    dirty_ = true;  structural_ = true;
    return id;
}

UI_Yoga::Node UI_Yoga::Text(Node parent, const UIStyle& s, const std::string& utf8,
                            Material* material, ModelData* quad, FontData* font, FontManager* fm)
{
    const Node id = MakeNode(impl_, s);
    NodeRec& r = impl_->nodes[id];
    r.material = material;
    r.quad     = quad;
    r.draws    = true;
    r.font     = 0;
    r.text     = utf8;   // для подписи узла в редакторе
    if (font && fm) {
        r.glyphs = fm->ShapeString(font, utf8);
        // Intrinsic-размер строки в ПИКСЕЛЯХ шрифта: Σ advance × line_height. Тайтовый бокс →
        // форма узла = форма шрифта; Yoga посадит его целиком, шейдер просто заливает.
        float adv = 0.0f;
        for (uint32_t gi : r.glyphs)
            if (gi < font->glyphs.size()) adv += static_cast<float>(font->glyphs[gi].advance);
        YGNodeStyleSetWidth (r.yg, adv);
        YGNodeStyleSetHeight(r.yg, static_cast<float>(font->line_height));
    }
    NodeRec& p = impl_->nodes[parent];
    YGNodeInsertChild(p.yg, r.yg, YGNodeGetChildCount(p.yg));
    p.children.push_back(id);
    dirty_ = true;  structural_ = true;
    return id;
}

void UI_Yoga::Clear()
{
    // Все узлы достижимы из корня (Box/Text всегда вставляются в родителя) → рекурсивный free
    // корня освобождает всё дерево. Созданные энтити тут НЕ трогаем (нет ctx) — они снимутся на
    // следующем Emit по impl_->created.
    if (impl_->root != kInvalid && impl_->nodes[impl_->root].yg)
        YGNodeFreeRecursive(impl_->nodes[impl_->root].yg);
    impl_->nodes.clear();
    impl_->root = kInvalid;
    dirty_ = true;  structural_ = true;
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

// Рекурсивный обход: аккумулируем абсолютный px-офсет, считаем rect → NDC-матрицу узла. create=true —
// СОЗДАЁМ энтити (структурный проход); create=false — МУТИРУЕМ Positions существующей энтити на месте
// (layout-правки: драг/ресайз — без recreate, иначе структурная мутация ECS каждый кадр роняет
// редактор, см. CLAUDE.md). depth: глубже = МЕНЬШЕ z (ближе) → потомок рисуется поверх предка
// независимо от порядка батчей (иначе z-конфликт «мигает»). UI-проход чистит depth, compare = LESS.
static void EmitNode(UI_Yoga::Impl* impl, EngineContext* ctx, ObjectManager* om, SceneData* scene_ptr,
                     const SceneName& scene_name, UI_Yoga::Node id, float ox, float oy,
                     float W, float H, int depth, bool create)
{
    constexpr float kZStep = 0.001f;   // шаг z на уровень (24 уровня = 0.024 — с запасом)

    NodeRec& r = impl->nodes[id];
    // Смещение из редактора добавляется ПОСЛЕ раскладки → узел (и его поддерево, т.к. (x,y) уходит
    // детям) едет, а соседи по flex не рефлоуятся.
    const float x = ox + YGNodeLayoutGetLeft(r.yg) + r.off_x;   // абсолютный px (top-left, y вниз)
    const float y = oy + YGNodeLayoutGetTop(r.yg)  + r.off_y;
    const float w = YGNodeLayoutGetWidth(r.yg);
    const float h = YGNodeLayoutGetHeight(r.yg);

    // px (y вниз) → NDC-прямоугольник. Считаем для ВСЕХ узлов (даже контейнеров) — чтобы гизмо мог
    // встать на любой выбранный узел; рисуем энтити только у r.draws.
    const float sx = (w / W) * 2.0f;          // масштаб X в NDC
    const float sy = (h / H) * 2.0f;          // масштаб Y
    const float L  = (x / W) * 2.0f - 1.0f;   // левый край в NDC
    const float T  = 1.0f - (y / H) * 2.0f;   // верхний край (флип: y вниз → NDC вверх)
    const float z  = 0.5f - static_cast<float>(depth) * kZStep + r.z_off;   // глубже = ближе, +bias слоя
    // Слепок центра для гизмо (все узлы).
    r.ndc_cx = L + sx * 0.5f;
    r.ndc_cy = T - sy * 0.5f;
    r.ndc_z  = z;
    r.emitted = true;

    if (r.draws && r.quad) {
        // Юнит-квад [0,1] раскладывается матрицей Positions: диагональ = масштаб, 4-й столбец
        // (w,d,h) = сдвиг (row-major, как scene.json).
        PositionProxy16 m{ sx,0,0,L,   0,sy,0,(T - sy),   0,0,1,z,   0,0,0,1 };

        if (create) {
            Entity e;
            // GeneratedComponent → SaveScene не пишет их в scene.json: узлы UI пересоздаёт
            // UI_Yoga::Emit из дерева каждую сессию, копия в файле = дубли/баги при загрузке.
            if (!r.glyphs.empty())
                e = ctx->CreateEntity(scene_name,
                        DrawComponent{ true, 1.0f, 0 },
                        ModelComponent{ r.quad },
                        MaterialComponent{ { r.material } },
                        m, UIComponent{}, GeneratedComponent{},
                        UITextComponent{ r.glyphs, r.font });
            else
                e = ctx->CreateEntity(scene_name,
                        DrawComponent{ true, 1.0f, 0 },
                        ModelComponent{ r.quad },
                        MaterialComponent{ { r.material } },
                        m, UIComponent{}, GeneratedComponent{});
            r.entity = e;  r.has_entity = true;
            impl->created.push_back(e);
        }
        else if (r.has_entity && om->Has<Positions>(scene_ptr, r.entity)) {
            WritePositions(om, scene_ptr, r.entity, m);   // мутация на месте (без recreate)
        }
    }

    for (UI_Yoga::Node c : r.children)
        EmitNode(impl, ctx, om, scene_ptr, scene_name, c, x, y, W, H, depth + 1, create);   // дети — от абсолюта
}

void UI_Yoga::Emit(EngineContext* ctx, float screen_w, float screen_h)
{
    if (!ctx) return;
    // Смена размера экрана инвалидирует раскладку так же, как dirty_ (см. Impl::last_w/h): без этого
    // ресайз не перекладывал UI — узлы держали NDC от стартового разрешения, отсюда «другой размер UI».
    const bool size_changed = (screen_w != impl_->last_w) || (screen_h != impl_->last_h);
    if (!dirty_ && !size_changed) return;
    impl_->last_w = screen_w;  impl_->last_h = screen_h;

    ObjectManager* om       = ctx->GetObjectManager();
    SceneData*     scene    = om->GetActiveScene();
    const SceneName scene_name = om->GetActiveSceneName();

    if (impl_->root == kInvalid || screen_w <= 0.0f || screen_h <= 0.0f) {
        // Дерева нет: снять энтити прошлой раскладки, если были.
        for (Entity e : impl_->created) ctx->DeleteEntity(scene_name, e);
        impl_->created.clear();
        structural_ = false;  dirty_ = false;  return;
    }

    YGNodeCalculateLayout(impl_->nodes[impl_->root].yg, screen_w, screen_h, YGDirectionLTR);

    if (structural_) {
        // Структурное изменение дерева (сборка/добавление узлов, редко) → полный recreate.
        for (Entity e : impl_->created) ctx->DeleteEntity(scene_name, e);
        impl_->created.clear();
        for (NodeRec& n : impl_->nodes) { n.has_entity = false; n.entity = static_cast<Entity>(-1); }
        EmitNode(impl_, ctx, om, scene, scene_name, impl_->root, 0.0f, 0.0f, screen_w, screen_h, 0, /*create*/true);
        structural_ = false;
    }
    else {
        // Только раскладка/смещение (драг гизмо, ресайз) → мутируем Positions существующих энтити на
        // месте, БЕЗ create/delete: иначе структурная мутация ECS каждый кадр против ForEach редактора
        // = краш (см. CLAUDE.md). Бонус: ресайз больше не тасует порядок батчей.
        EmitNode(impl_, ctx, om, scene, scene_name, impl_->root, 0.0f, 0.0f, screen_w, screen_h, 0, /*create*/false);
    }
    dirty_ = false;
}
