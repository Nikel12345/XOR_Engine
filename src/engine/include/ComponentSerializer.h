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
#include <map>
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
    // Аксессоры — каптурлесс-лямбды из макросов AOS_NUM/SOA_NUM/AOS_STR (объявлены ниже).
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


// Словарь имён ассетов в шапке scene.json. Колонка ассета хранит ИНДЕКС в списке, а не имя:
// у сцены на 1M кубов имя модели повторяется миллион раз при дюжине разных значений, и то же
// у материалов. Списков несколько, по одному на вид ассета ("models", "materials"), потому что
// имена живут в РАЗНЫХ пространствах менеджеров: общий пул склеил бы модель и материал с
// одинаковым именем в одну запись — работать бы работало, но список перестал бы читаться.
//
// Индекс — ОПТИМИЗАЦИЯ, а не схема. В ячейке строковой колонки законны оба вида: строка значит
// «имя как есть», число — «индекс в списке». Поэтому сцены, сохранённые до словаря, грузятся без
// конвертации, а написанную руками сцену можно вообще не пропускать через словарь.
//
// Словарь живёт РОВНО одно сохранение/загрузку — это не реестр движка, а часть формата файла.
class ScenePool {
public:
    // Список имён одного вида ассета. index — только для записи, чтение по нему не ходит.
    struct List {
        std::vector<std::string>                  names;
        std::unordered_map<std::string, uint32_t> index;
        uint32_t Intern(const std::string& name);
    };

    // Список по имени; заводится при первом обращении. Берётся ОДИН раз на колонку, не на ячейку.
    // Контейнер узловой намеренно: ссылка на List обязана пережить появление соседнего списка.
    List& operator[](const std::string& list_name) { return lists_[list_name]; }
    List* Find(const std::string& list_name);

    // Ячейка строковой колонки → имя (nullptr, если не разрешилась). list == nullptr — колонка
    // без словаря: тогда законна только строка.
    const char* Cell(const List* list, yyjson_val* v);

    void Write(yyjson_mut_doc* doc, yyjson_mut_val* root) const;   // списки в шапку файла
    void Read(yyjson_val* root);                                   // списки из шапки файла

    // Рассогласований словаря: индекс мимо списка, нечитаемая ячейка, не-строка в самом списке.
    // Считаем, а не логируем на месте: битый файл на миллионе сущностей залил бы лог миллионом строк.
    uint32_t Misses() const { return misses_; }

private:
    std::map<std::string, List> lists_;   // упорядоченный: порядок списков в файле детерминирован
    uint32_t misses_ = 0;
};

// Имя списка-словаря для ассетного вида поля; nullptr — поле пишется строкой как есть.
// Вид поля УЖЕ говорит, из какого менеджера имя (AssetModel рисует комбо моделей в инспекторе),
// второго объявления того же факта в схеме не заводим.
constexpr const char* FieldPoolName(FieldKind k)
{
    return k == FieldKind::AssetModel ? "models" : nullptr;
}

//  Аксессоры полей: пара (get, set) каптурлесс-лямбдами. Публичны (как AddDefault*) — ими
//  пишутся и регистрации верхних слоёв: игра объявляет СВОЙ компонент в своих файлах, движок
//  для этого не правится.
//  AoS — поле по цепочке членов компонента T; SoA — колонка col хранилища S.
//  set приводит double к фактическому типу поля (float/int/uint32_t/bool).
#define AOS_NUM(T, path) \
    [](Archetype& a, size_t i) -> double { return (double)(*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, double v) { auto& r = (*a.get_array<T>())[i].path; r = (std::decay_t<decltype(r)>)v; }
#define SOA_NUM(S, col) \
    [](Archetype& a, size_t i) -> double { return (double)a.get_array<S>()->data.col[i]; }, \
    [](Archetype& a, size_t i, double v) { auto& r = a.get_array<S>()->data.col[i]; r = (std::decay_t<decltype(r)>)v; }
#define AOS_STR(T, path) \
    [](Archetype& a, size_t i) -> const std::string& { return (*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, std::string v) { (*a.get_array<T>())[i].path = std::move(v); }

//  ВЫЧИСЛЯЕМОЕ поле — только геттер, сеттер nullptr: величина, которой в данных нет, а есть
//  расчёт по ним. Отдельного флага не нужно, всё выводится из отсутствия сеттера: Save её не
//  пишет (иначе в файле окажется колонка, которую Load некуда положить), Load не читает,
//  UI рисует нередактируемой (меткой). Выражение видит ряд компонента как `c` (у SoA —
//  хранилище `c` и индекс `i`) и может звать методы, а не только читать члены.
#define AOS_CALC(T, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = (*a.get_array<T>())[i]; return (double)(expr); }, \
    nullptr
#define SOA_CALC(S, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = a.get_array<S>()->data; return (double)(expr); }, \
    nullptr
//  Строковый вариант: производная величина не обязана быть числом (таблица, режим, диагностика).
//  Он же — единственный способ показать то, чьё КОЛИЧЕСТВО лежит в данных: схема описывает тип
//  компонента и переменного числа полей выразить не может, а геттер — обычный код, он может всё.
#define AOS_CALC_STR(T, expr) \
    [](Archetype& a, size_t i) -> const std::string& { \
        auto& c = (*a.get_array<T>())[i]; \
        thread_local std::string s; s = (expr); return s; }, \
    nullptr


struct ComponentSpec {
    std::string     name;       // имя в файле = ключ объекта компонента, напр. "Model"
    std::type_index sig_type;   // type_index, идущий в сигнатуру архетипа (для SoA — тип хранилища)

    void (*add_default)(Archetype&) = nullptr;         // ensure_component<T> + один дефолтный ряд
    std::vector<FieldSpec> fields;                     // пусто → тег без данных
    void (*after_edit)(Archetype&, size_t) = nullptr;  // побочный эффект правки в UI (needsUpdate у света)

    // Escape hatch: заданы → генераторы по fields не используются.
    void (*custom_save)(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*, ScenePool*) = nullptr;
    void (*custom_load)(Archetype&, yyjson_val*, size_t, ScenePool*) = nullptr;

    // Память → json: записать ВЕСЬ компонент архетипа (count строк) КОЛОНКАМИ по полям в comp.
    // pool — словарь имён ассетов шапки файла (см. ScenePool); nullptr = без словаря,
    // строковые колонки пишутся/читаются именами.
    void Save(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp, ScenePool* pool) const;
    // json → память: add_default × count + сеттеры по колонкам comp (count-гвард: недостающие/
    // короткие колонки оставляют дефолт, длинные усекаются). comp==nullptr → count дефолтных
    // рядов — этим же путём форма создания получает «пустой» компонент. ИНВАРИАНТ вызова:
    // сущности уже добавлены в arch.entities (строки дописываются в хвост колонок), как в
    // ObjectManager::LoadScene.
    void Load(Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool) const;
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
