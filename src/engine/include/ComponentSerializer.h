#pragma once
// Реестр спецификаций компонентов. Мост «строка из файла ↔ конкретный C++ тип»:
// у архетипа компонент стёрт до IComponentArray (виден только type_index), а в файле
// он — строка-имя. Раньше на компонент регистрировалась пара рукописных {save, load};
// теперь компонент ДЕКЛАРИРУЕТ схему полей (fields), а save/load — генерируемые
// интерпретаторы схемы. Та же схема — источник для инспектора и формы создания энтити
// (generic ImGui-рендерер в UI-слое, фаза 2): поле объявляется ОДИН раз, диапазоны и
// дефолты не расходятся между файлом и UI. custom_save/custom_load — escape hatch для
// невыразимого схемой (Material: зубчатый массив имён). Открытая регистрация: движок
// регистрирует свои компоненты здесь, верхние слои (физика/игра) могут добавить свои.
#include <string>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include "BaseComponents.h"
#include "CommandId.h"   // FieldSpec::cmd — чем уходит правка поля; сам InputManager сюда не тянется
#include "yyjson.h"

// Вид поля: json-тип колонки (real/uint/bool/str) и виджет UI (Drag/Checkbox/Input/комбо).
// Asset* — строка-имя ассета: сериализуется как Str, UI рисует комбо из менеджера.
// Angle — радианы в данных/файле (как F32), но слайдер UI в градусах; lo/hi у него — ГРАДУСЫ.
enum class FieldKind : uint8_t { F32, U32, Bool, Str, AssetModel, Angle };

// Сколько ПОДРЯД идущих полей рисуются ОДНИМ виджетом. Ставится на первое поле группы
// (.Group(...)), остальные рендерер пропускает — они уже нарисованы.
//
// Раньше группы UI угадывал по именам колонок ("r","g","b" → ColorEdit3, "x","y","z" →
// DragFloat3) и на Transform ошибался: там x,y,z — это m00,m01,m02 матрицы, а перенос лежит
// в w,d,h. Догадок больше нет: что с чем рисуется, объявляет схема, UI только исполняет.
enum class FieldGroup : uint8_t { None, Vec3, Color3, Mat4 };

constexpr size_t FieldGroupSize(FieldGroup g)
{
    switch (g) {
    case FieldGroup::Vec3:
    case FieldGroup::Color3: return 3;
    case FieldGroup::Mat4:   return 16;
    default:                 return 1;
    }
}

struct FieldSpec {
    const char* key = nullptr;     // json-ключ колонки И лейбл поля в инспекторе
    FieldKind   kind = FieldKind::F32;

    // Тип-стёртый доступ к полю строки row (индекс в колонках архетипа). Числовые виды
    // ходят через double (общий канал для f32/u32/int/bool), строковые — через std::string.
    // Аксессоры — каптурлесс-лямбды из макросов AOS_NUM/SOA_NUM/AOS_STR (см. .cpp).
    double (*get_num)(Archetype&, size_t) = nullptr;
    void   (*set_num)(Archetype&, size_t, double) = nullptr;
    const std::string& (*get_str)(Archetype&, size_t) = nullptr;
    void   (*set_str)(Archetype&, size_t, std::string) = nullptr;

    // Диапазон значения: драг в UI и, при clamp_on_load, жёсткий кламп на загрузке
    // (пример: cascade_count 1..MAX_CASCADES — раньше кламп дублировался в load и в UI).
    // lo==hi → диапазон не задан.
    float lo = 0, hi = 0;
    float speed = 0.05f;           // шаг драга в UI
    bool  clamp_on_load = false;
    // Поле видно, но не редактируется generic-рендерером. Для полей, чья прямая запись рвёт
    // инварианты движка: Parent.parent (обратный индекс scene->children), Model.name у ЖИВОЙ
    // энтити (смена модели = резолв указателя + пересборка батчей → команда).
    bool  ui_readonly = false;

    // Правка поля у ЖИВОЙ энтити уходит ЭТОЙ командой в sim-поток ВМЕСТО записи в колонку.
    // Для полей, чья запись меняет состояние движка мимо ECS: Draw.visible — состав дерева
    // батчей, имя ассета — ключ батча. У staging-черновика (форма создания) команде некому
    // адресоваться и он не в батчах — там всегда прямая запись, cmd игнорируется.
    //
    // ВАЖНО: значение возвращается в UI через кадр-другой (пишет sim), поэтому вешать cmd
    // можно на ДИСКРЕТНЫЕ виджеты — чекбокс, комбо. На драг-слайдер нельзя: он перечитывает
    // колонку каждый кадр и без локального кэша «отправленного» просто не сдвинется.
    CommandId cmd = CommandId::None;

    // Группа, которую открывает это поле (см. FieldGroup), и подпись её виджета.
    // group_label == nullptr → подписью служит key самого поля.
    FieldGroup  group = FieldGroup::None;
    const char* group_label = nullptr;

    static FieldSpec Num(const char* key, FieldKind kind,
                         double (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, double),
                         float lo = 0, float hi = 0, float speed = 0.05f);
    static FieldSpec Str(const char* key,
                         const std::string& (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, std::string),
                         FieldKind kind = FieldKind::Str);

    // Модификаторы-цепочки к фабрикам: FieldSpec::Num(...).Clamp() / .ReadOnly() / .Cmd(id) / .Group(g)
    FieldSpec&& Clamp()    && { clamp_on_load = true; return std::move(*this); }
    FieldSpec&& ReadOnly() && { ui_readonly   = true; return std::move(*this); }
    FieldSpec&& Cmd(CommandId id) && { cmd = id; return std::move(*this); }
    FieldSpec&& Group(FieldGroup g, const char* label = nullptr) &&
    { group = g; group_label = label; return std::move(*this); }
};

// Дефолтный ряд компонента. Истина о значениях по умолчанию — member-инициализаторы
// самого компонента (T{} / Proxy{}), а НЕ дубль в схеме: недостающая в файле колонка
// просто оставляет их. Шаблоны публичны — ими пользуются и регистрации верхних слоёв.
template<typename T>
void AddDefaultAoS(Archetype& arch) { arch.ensure_component<T>(); arch.get_array<T>()->add(T{}); }
template<typename SoA, typename Proxy>
void AddDefaultSoA(Archetype& arch) { arch.ensure_component<SoA>(); arch.get_array<SoA>()->add(Proxy{}); }

struct ComponentSpec {
    std::string     name;       // имя в файле = ключ объекта компонента, напр. "Model"
    std::type_index sig_type;   // type_index, идущий в сигнатуру архетипа (для SoA — тип хранилища)

    void (*add_default)(Archetype&) = nullptr;         // ensure_component<T> + один дефолтный ряд
    std::vector<FieldSpec> fields;                     // пусто → тег без данных
    void (*after_edit)(Archetype&, size_t) = nullptr;  // побочный эффект правки в UI (needsUpdate у света)

    // Escape hatch: заданы → генераторы по fields не используются.
    void (*custom_save)(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*) = nullptr;
    void (*custom_load)(Archetype&, yyjson_val*, size_t) = nullptr;

    // Память → json: записать ВЕСЬ компонент архетипа (count строк) КОЛОНКАМИ по полям в comp.
    void Save(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp) const;
    // json → память: add_default × count + сеттеры по колонкам comp (count-гвард: недостающие/
    // короткие колонки оставляют дефолт, длинные усекаются). comp==nullptr → count дефолтных
    // рядов — этим же путём форма создания получает «пустой» компонент. ИНВАРИАНТ вызова:
    // сущности уже добавлены в arch.entities (строки дописываются в хвост колонок), как в
    // ObjectManager::LoadScene.
    void Load(Archetype& arch, yyjson_val* comp, size_t count) const;
};

class ComponentSpecRegistry {
public:
    static ComponentSpecRegistry& Get();

    void Register(ComponentSpec s);
    const ComponentSpec* ByName(const std::string& name) const;   // для загрузки
    const ComponentSpec* ByType(std::type_index t) const;         // для сохранения
    // Все зарегистрированные компоненты (порядок регистрации) — чекбоксы формы создания энтити.
    const std::vector<ComponentSpec>& All() const { return specs_; }

private:
    std::vector<ComponentSpec>                    specs_;
    std::unordered_map<std::string, size_t>       by_name_;
    std::unordered_map<std::type_index, size_t>   by_type_;
};

// Регистрирует спецификации встроенных (движковых) компонентов. Идемпотентна —
// повторный вызов ничего не дублирует. Звать один раз на старте движка.
void RegisterBuiltinComponentSpecs();
