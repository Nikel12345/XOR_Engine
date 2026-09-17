#pragma once
#include <cstdint>
#include <string>
#include <vector>

class EngineContext;
class FontManager;
struct FontData;

enum class UIDir     : uint8_t { Row, Column };
enum class UIJustify : uint8_t { Start, Center, End, SpaceBetween };   // вдоль главной оси
enum class UIAlign   : uint8_t { Start, Center, End, Stretch };        // поперёк главной оси
enum class UISize    : uint8_t { Auto, Points, Percent, Grow };

struct UIStyle {
    UIDir     dir     = UIDir::Column;
    UIJustify justify = UIJustify::Start;   // распределение детей по главной оси
    UIAlign   align   = UIAlign::Stretch;   // выравнивание детей поперёк (CSS-дефолт flex)
    UISize    wmode   = UISize::Auto;   float w = 0.0f;   // Points=px, Percent=% родителя, Grow=flex-grow
    UISize    hmode   = UISize::Auto;   float h = 0.0f;
    float     grow    = 0.0f;   // flex-grow (заполнить свободное место главной оси)
    float     padding = 0.0f;   // все края, px
    float     margin  = 0.0f;   // все края, px
    float     gap     = 0.0f;   // зазор между детьми, px
};

class UI_Yoga {
public:
    UI_Yoga();
    ~UI_Yoga();

    using Node = uint32_t;
    static constexpr Node kInvalid = 0xFFFFFFFFu;

    Node Root(const UIStyle& s);                                        // корень = холст экрана
    Node Box (Node parent, const UIStyle& s, const std::string& material, const std::string& quad);
    Node Text(Node parent, const UIStyle& s, const std::string& utf8,
              const std::string& material, const std::string& quad, FontData* font, FontManager* fm);

    void Clear();                        // снести дерево (энтити снимутся на следующем Emit)
    void MarkDirty() { dirty = true; }  // ресайз окна / смена контента → пересчёт на следующем Emit
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

    struct Impl;

private:
    Impl* impl_ = nullptr;   // прячет Yoga-типы (pimpl)
    bool  dirty = true;      // нужен Emit (раскладка/смещение изменились)
    bool  structural_ = true; // дерево изменилось (добавили/убрали узлы) → нужен полный recreate энтити.
                              // Иначе Emit мутирует Positions существующих на месте (без recreate —
                              // recreate каждый кадр драга роняет редактор, см. CLAUDE.md).
};
