#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Aliases.h"
#include "ComponentStorage.h"
#include "ResourceId.h"

class EngineContext;
class FontManager;
class ObjectManager;
struct SceneData;
// Yoga держим ТОЛЬКО в .cpp: заголовок форвардит opaque-типы узла и конфига (YGNodeRef = YGNode*).
struct YGNode;
struct YGConfig;

enum class UIDir     : uint8_t { Row, Column };
enum class UIJustify : uint8_t { Start, Center, End, SpaceBetween };   // вдоль главной оси
enum class UIAlign   : uint8_t { Start, Center, End, Stretch };        // поперёк главной оси
enum class UISize    : uint8_t { Auto, Points, Percent, Grow };

struct UIEdges {   // px, порядок как в CSS-сокращении
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;
    bool operator==(const UIEdges&) const = default;
};

struct UIStyle {
    UIDir     dir     = UIDir::Column;
    UIJustify justify = UIJustify::Start;   // распределение детей по главной оси
    UIAlign   align   = UIAlign::Stretch;   // выравнивание детей поперёк (CSS-дефолт flex)
    UISize    wmode   = UISize::Auto;   float w = 0.0f;   // Points=px, Percent=% родителя, Grow=flex-grow
    UISize    hmode   = UISize::Auto;   float h = 0.0f;
    float     grow    = 0.0f;   // flex-grow (заполнить свободное место главной оси)
    UIEdges   padding;
    UIEdges   margin;
    float     gap     = 0.0f;   // зазор между детьми, px
};

// Текст узла после ShapeString — всё, что нужно переносу, без FontData. Живёт отдельной аллокацией:
// её адрес Yoga держит контекстом measure-функции, а UI_Manager::nodes переезжает при росте.
struct UITextShape {
    struct Glyph {
        uint32_t code;       // glyph_index или UI_LINE_BREAK_CODE
        float    advance;    // px
        bool     space;      // здесь можно перенести строку
        bool operator==(const Glyph&) const = default;
    };
    std::vector<Glyph> glyphs;
    float              line_height = 0.0f;
};

struct UINode {
    YGNode*               yg       = nullptr;
    UIStyle               style;                 // применённый к yg — его же отдаёт GetNodeStyle
    bool                  draws    = false;      // создавать ли энтити (контейнер без материала — нет)
    std::string           material;               // имена ассетов (см. UI_Manager::Box) — как в компонентах
    std::string           quad;
    std::unique_ptr<UITextShape> shape;          // есть → текстовый узел (UITextComponent)
    std::vector<uint32_t> glyphs;                // поток для UITextComponent: shape, перенесённый по ширине узла
    FontId                font;
    // Подпись узла в редакторе. SetText её не трогает: NodeLabel копирует строку с UI-потока, и
    // перезапись на sim дала бы чтение освобождённого буфера.
    std::string           text;
    std::string           id;                    // id элемента UI-файла: подпись в редакторе
    std::vector<uint32_t> children;              // индексы в UI_Manager::nodes
    // Правка из редактора: смещение поверх посчитанной раскладки (offset px XY + z-bias слоя).
    float                 off_x = 0.0f, off_y = 0.0f, z_off = 0.0f;
    glm::mat4             local{ 1.0f };
    // Слепок последнего Emit (центр в NDC + z, размер раскладки в px) — для гизмо и инспектора. Пишет
    // sim, читает UI (benign).
    float                 ndc_cx = 0.0f, ndc_cy = 0.0f, ndc_z = 0.5f;
    float                 layout_w = 0.0f, layout_h = 0.0f;
    bool                  emitted = false;
    // Энтити этого узла (если рисуется) — для мутации Positions на месте на layout-правках (без recreate).
    Entity                entity = static_cast<Entity>(-1);
    bool                  has_entity = false;
};

class UI_Manager {
public:
    UI_Manager();
    ~UI_Manager();

    using Node = uint32_t;
    static constexpr Node kInvalid = 0xFFFFFFFFu;

    Node Root(const UIStyle& s);                                        // корень = холст экрана
    Node Box (Node parent, const UIStyle& s, const std::string& material, const std::string& quad);
    Node Text(Node parent, const UIStyle& s, const std::string& utf8,
              const std::string& material, const std::string& quad, FontId font, const FontManager* fm);

    // Дерево из UI-файла (docs/ui.md, «UI-файл»). Документ проверяется целиком до сборки: при ошибках
    // все они уходят в лог с файл:строка, а прежнее дерево остаётся.
    bool Load(const FontManager* fm, const std::string& path);

    void Clear();                        // снести дерево (энтити снимутся на следующем Emit)
    bool HasTree() const;

    void Reset();

    void Emit(EngineContext* ctx, float screen_w, float screen_h);

    Node        RootNode() const;
    uint32_t    ChildCount(Node n) const;
    Node        ChildAt(Node n, uint32_t i) const;
    std::string NodeLabel(Node n) const;

    // ── Правка узла из редактора (гизмо XY + кнопки Z). Пишет sim-поток (через команду). ──
    // += приращение: offset px по XY (применяется пост-layout в EmitNode → узел с поддеревом едет,
    // соседи нет) + z-bias (слой). Сам ставит dirty → пересчёт на следующем Emit.
    void NudgeNode(Node n, float ddx, float ddy, float ddz);
    bool GetNodeNdc(Node n, float& ndc_x, float& ndc_y, float& z) const;
    void GetOffset(Node n, float& dx, float& dy, float& dz) const;
    void SetNodeTransform(Node n, const float m[16]);
    bool GetNodeTransform(Node n, float m[16]) const;
    // Стиль целиком (размеры, отступы, выравнивание). Правка не структурная: Emit переложит дерево,
    // заново перенесёт текст и перепишет Positions на месте. У текстового узла padding всегда 0.
    void SetNodeStyle(Node n, const UIStyle& s);
    bool GetNodeStyle(Node n, UIStyle& s) const;
    bool GetNodeLayoutSize(Node n, float& w, float& h) const;   // итог последней раскладки, px
    bool IsTextNode(Node n) const;
    // Строка текстового узла. Правка не структурная: Emit переложит дерево и перепишет текст
    // энтити на месте. Та же строка — ничего не делает, так что звать можно каждый тик.
    void SetText(Node n, const std::string& utf8, const FontManager* fm);
    // Материал узла по имени, "" — без материала (у текста недопустимо). Энтити, которая уже есть,
    // меняет материал на месте; появление или исчезновение энтити — структурная правка.
    void SetNodeMaterial(EngineContext* ctx, Node n, const std::string& material);
    bool GetNodeEntity(Node n, uint32_t& entity) const;          // энтити последнего Emit

private:
    Node MakeNode(const UIStyle& s);
    void EmitNode(EngineContext* ctx, ObjectManager* om, SceneData* scene_ptr, const SceneName& scene_name,
                  Node id, float ox, float oy, float W, float H, int& order, bool create);

    YGConfig*           cfg = nullptr;
    std::vector<UINode> nodes;
    std::vector<Entity> created;                 // энтити прошлого Emit (снимаются на пересборке)
    Node                root = kInvalid;
    // Размер экрана прошлой раскладки. Раскладка зависит от него (points-узлы дают разную ДОЛЮ экрана
    // при разном разрешении) → его смена = layout-инвалидация, даже без dirty. -1 = ещё не раскладывали.
    float               last_w = -1.0f, last_h = -1.0f;
    bool                dirty = true;            // нужен Emit (раскладка/смещение изменились)
    bool                structural = true;       // дерево изменилось (добавили/убрали узлы) → нужен полный recreate энтити.
                                                 // Иначе Emit мутирует Positions существующих на месте (без recreate —
                                                 // recreate каждый кадр драга роняет редактор, см. CLAUDE.md).
};
