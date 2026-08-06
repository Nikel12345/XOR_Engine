#pragma once
#include <cstdint>
#include <string>
#include <vector>

class EngineContext;
class FontManager;
struct FontData;

// Мост между flex-раскладкой (Yoga) и ECS-энтити UI. Игра описывает ДЕРЕВО боксов (стили +
// текст), Yoga считает прямоугольники, Emit создаёт UI-энтити с Positions = посчитанный NDC-rect.
// Сам Yoga наружу НЕ течёт (pimpl) — как SDL_ttf в FontManager. Ручное создание UI-энтити
// (ctx->CreateEntity с UIComponent) по-прежнему работает: это просто второй, императивный путь.
//
// Модель — «всё есть бокс» (как div/span/text в HTML): контейнер (без материала) не рисуется,
// только раскладывает детей; Box с материалом = панель (фон); Text = бокс с intrinsic-размером
// из метрик шрифта (тайтовый прямоугольник по строке). Т.к. Yoga считает в ПИКСЕЛЯХ кадра и rect
// переводится в NDC по реальному размеру экрана, форма получается aspect-корректной сама —
// шейдеру не нужны un-squish/anchor.

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

    // Построение дерева (обычно в init игры). Root начинает новое дерево (сносит прежнее).
    // Материал/модель узла — ПО ИМЕНИ: узел кладёт их в ModelComponent/MaterialComponent как есть,
    // а те держат имена (резолв — на сборке батчей). Пустое имя материала → чистый контейнер.
    Node Root(const UIStyle& s);                                        // корень = холст экрана
    Node Box (Node parent, const UIStyle& s, const std::string& material, const std::string& quad);
    Node Text(Node parent, const UIStyle& s, const std::string& utf8,
              const std::string& material, const std::string& quad, FontData* font, FontManager* fm);

    void Clear();                        // снести дерево (энтити снимутся на следующем Emit)
    void MarkDirty() { dirty_ = true; }  // ресайз окна / смена контента → пересчёт на следующем Emit
    bool HasTree() const;

    // Сцена снесена извне (LoadScene: SceneData::clear) — вместе с нашими энтити. UI уходит
    // ЦЕЛИКОМ: дерево тоже сносим, ничего не складывая. Отличие от Clear() — энтити НЕ
    // удаляем: их уже нет, а их id сцена раздала заново (clear сбрасывает next_entity_id),
    // так что удаление по ним попало бы в чужой объект. Новый UI придёт с загрузкой сцены,
    // когда дерево начнёт сохраняться в её файлы.
    void Reset();

    // Проход эмита (зовётся Engine в начале PrepareFunc, до сборки батчей). Если грязно: сносит
    // энтити прошлой раскладки, считает layout под (screen_w, screen_h), создаёт энтити в АКТИВНОЙ
    // сцене, пишет rect в Positions. Иначе no-op.
    void Emit(EngineContext* ctx, float screen_w, float screen_h);

    // ── Обход дерева для редактора (UI-вкладка панели иерархии). ──
    Node        RootNode() const;                     // корень дерева (kInvalid, если пусто)
    uint32_t    ChildCount(Node n) const;             // число прямых детей узла
    Node        ChildAt(Node n, uint32_t i) const;    // i-й ребёнок (kInvalid при выходе за границы)
    std::string NodeLabel(Node n) const;              // подпись для дерева ("UI Root"/"Panel"/"Text: …"/"Group")

    // ── Правка узла из редактора (гизмо XY + кнопки Z). Пишет sim-поток (через команду). ──
    // += приращение: offset px по XY (применяется пост-layout в EmitNode → узел с поддеревом едет,
    // соседи нет) + z-bias (слой). Сам ставит dirty → пересчёт на следующем Emit.
    void NudgeNode(Node n, float ddx, float ddy, float ddz);
    // Центр узла в NDC + z из последнего Emit — для постановки гизмо. false, если ещё не эмитился.
    bool GetNodeNdc(Node n, float& ndc_x, float& ndc_y, float& z) const;
    // Текущее накопленное смещение узла (для показа в инспекторе).
    void GetOffset(Node n, float& dx, float& dy, float& dz) const;

    // Определение в .cpp (pimpl). Форвард публичный, чтобы свободные хелперы в UI_Yoga.cpp могли
    // принимать Impl* — сам тип в заголовке неполный, наружу ничего из Yoga не раскрывает.
    struct Impl;

private:
    Impl* impl_ = nullptr;   // прячет Yoga-типы (pimpl)
    bool  dirty_ = true;      // нужен Emit (раскладка/смещение изменились)
    bool  structural_ = true; // дерево изменилось (добавили/убрали узлы) → нужен полный recreate энтити.
                              // Иначе Emit мутирует Positions существующих на месте (без recreate —
                              // recreate каждый кадр драга роняет редактор, см. CLAUDE.md).
};
