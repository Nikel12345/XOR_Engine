#pragma once
// Реестр спецификаций компонентов — мост «строка из файла <-> конкретный C++ тип»: у архетипа
// компонент стёрт до IComponentArray (виден только type_index), а в файле он всего лишь имя.
// Компонент ДЕКЛАРИРУЕТ схему полей, save/load — интерпретаторы этой схемы. Та же схема кормит
// инспектор и форму создания сущности, поэтому диапазоны и дефолты не расходятся между файлом и
// UI. custom_save/custom_load — escape hatch для невыразимого схемой (Material). Регистрация
// открытая: движок объявляет свои компоненты, верхние слои (физика/игра) — свои.
#include <string>
#include <vector>
#include <map>
#include <typeindex>
#include <functional>
#include <unordered_map>
#include "ComponentStorage.h"
#include "CommandId.h"
#include "yyjson.h"

// Вид поля: json-тип колонки и виджет UI. Asset* — имя ассета: в файле строка, в UI комбо из
// менеджера. Angle — радианы в данных и в файле (как F32), градусы только в слайдере, поэтому
// lo/hi у него задают в ГРАДУСАХ.
enum class FieldKind : uint8_t { F32, U32, Bool, Str, AssetModel, Angle };

// Сколько ПОДРЯД идущих полей рисуются ОДНИМ виджетом; ставится на первое поле группы, остальные
// рендерер пропускает. Объявляется явно, а не угадывается по именам колонок: у Transform x,y,z —
// это m00,m01,m02, а не вектор, и перенос лежит в w,d,h.
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

    // Тип-стёртый доступ к полю одной строки архетипа. Числовые виды ходят через double — он
    // общий канал для f32/u32/int/bool, — строковые через std::string. Сами аксессоры делают
    // макросы ниже: каптурлесс-лямбды, поэтому это обычные указатели на функции.
    double (*get_num)(Archetype&, size_t) = nullptr;
    void   (*set_num)(Archetype&, size_t, double) = nullptr;
    // std::function, а не указатель: поле-ссылка на ресурс регистрируется слоем, который знает
    // менеджер, и переводит id в имя захватом. EngineEcs менеджеры не называет.
    std::function<const std::string&(Archetype&, size_t)> get_str;
    std::function<void(Archetype&, size_t, std::string)>  set_str;

    // Диапазон: драг в UI и, при clamp_on_load, жёсткий кламп на загрузке — одно объявление на
    // оба пути. lo==hi значит «диапазон не задан».
    float lo = 0, hi = 0;
    float speed = 0.05f;           // шаг драга в UI
    bool  clamp_on_load = false;
    // Видно, но не редактируется: прямая запись этого поля порвала бы инварианты движка
    // (Parent.parent — обратный индекс scene->children).
    bool  ui_readonly = false;

    // Правка поля у ЖИВОЙ сущности уходит ЭТОЙ командой в sim-поток ВМЕСТО записи в колонку —
    // для полей, чья запись меняет состояние движка мимо ECS (Draw.visible — состав дерева
    // батчей, имя ассета — ключ батча). Черновик формы создания в батчах не состоит и адресовать
    // команду ему некому: там всегда прямая запись, cmd игнорируется.
    //
    // ВАЖНО: значение вернётся в UI через кадр-другой — его пишет sim. Поэтому cmd вешают на
    // ДИСКРЕТНЫЕ виджеты (чекбокс, комбо), а на драг-слайдер нельзя: он перечитывает колонку
    // каждый кадр и без локального кэша отправленного просто не сдвинется.
    CommandId cmd = CommandId::None;

    // Группа, которую ОТКРЫВАЕТ это поле, и подпись её виджета (nullptr — подписью служит key).
    FieldGroup  group = FieldGroup::None;
    const char* group_label = nullptr;

    static FieldSpec Num(const char* key, FieldKind kind,
                         double (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, double),
                         float lo = 0, float hi = 0, float speed = 0.05f);
    static FieldSpec Str(const char* key,
                         std::function<const std::string&(Archetype&, size_t)> get,
                         std::function<void(Archetype&, size_t, std::string)> set,
                         FieldKind kind = FieldKind::Str);

    FieldSpec&& Clamp()    && { clamp_on_load = true; return std::move(*this); }
    FieldSpec&& ReadOnly() && { ui_readonly   = true; return std::move(*this); }
    FieldSpec&& Cmd(CommandId id) && { cmd = id; return std::move(*this); }
    FieldSpec&& Group(FieldGroup g, const char* label = nullptr) &&
    { group = g; group_label = label; return std::move(*this); }
};

// Дефолтный ряд компонента. Значения по умолчанию живут в member-инициализаторах самого
// компонента (T{} / Proxy{}), а не дублем в схеме: недостающая в файле колонка просто их
// оставляет. Публичны — ими пользуются и регистрации верхних слоёв.
template<typename T>
void AddDefaultAoS(Archetype& arch) { arch.ensure_component<T>(); arch.get_array<T>()->add(T{}); }
template<typename SoA, typename Proxy>
void AddDefaultSoA(Archetype& arch) { arch.ensure_component<SoA>(); arch.get_array<SoA>()->add(Proxy{}); }


// Словарь имён ассетов в шапке scene.json: колонка ассета хранит ИНДЕКС в списке, а не имя — у
// сцены на 1M кубов имя модели повторяется миллион раз при дюжине разных значений. Списков
// несколько, по одному на вид ассета, потому что имена живут в разных пространствах менеджеров.
//
// Индекс — ОПТИМИЗАЦИЯ, а не схема: в ячейке строковой колонки законны оба вида, строка значит
// «имя как есть», число — «индекс в списке». Поэтому сцену можно написать руками, вообще не
// заводя словаря. Живёт ровно одно сохранение или загрузку — это часть формата файла, не реестр.
class ScenePool {
public:
    // Список имён одного вида ассета; index нужен только записи, чтение по нему не ходит.
    struct List {
        std::vector<std::string>                  names;
        std::unordered_map<std::string, uint32_t> index;
        uint32_t Intern(const std::string& name);
    };

    // Список по имени, заводится при первом обращении. Берут его ОДИН раз на колонку, не на
    // ячейку, и контейнер узловой намеренно: ссылка обязана пережить появление соседнего списка.
    List& operator[](const std::string& list_name) { return lists_[list_name]; }
    List* Find(const std::string& list_name);

    // Ячейка строковой колонки -> имя (nullptr, если не разрешилась). list == nullptr — колонка
    // без словаря: тогда законна только строка.
    const char* Cell(const List* list, yyjson_val* v);

    void Write(yyjson_mut_doc* doc, yyjson_mut_val* root) const;
    void Read(yyjson_val* root);

    // Рассогласования словаря (индекс мимо списка, нечитаемая ячейка, не-строка в самом списке)
    // считаем, а не логируем на месте: битый файл на миллионе сущностей дал бы миллион строк.
    uint32_t Misses() const { return misses_; }

private:
    std::map<std::string, List> lists_;   // упорядоченный: порядок списков в файле детерминирован
    uint32_t misses_ = 0;
};

// Имя списка-словаря для ассетного вида поля; nullptr — поле пишется строкой как есть. Вид поля
// УЖЕ говорит, из какого менеджера имя, второго объявления того же факта в схеме нет.
constexpr const char* FieldPoolName(FieldKind k)
{
    return k == FieldKind::AssetModel ? "models" : nullptr;
}

//  Аксессоры поля парой (get, set). AoS — поле по цепочке членов компонента T, SoA — колонка col
//  хранилища S; set приводит double к фактическому типу поля. Публичны, как AddDefault*: игра
//  объявляет свой компонент у себя, движок для этого не правится.
#define AOS_NUM(T, path) \
    [](Archetype& a, size_t i) -> double { return (double)(*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, double v) { auto& r = (*a.get_array<T>())[i].path; r = (std::decay_t<decltype(r)>)v; }
#define SOA_NUM(S, col) \
    [](Archetype& a, size_t i) -> double { return (double)a.get_array<S>()->data.col[i]; }, \
    [](Archetype& a, size_t i, double v) { auto& r = a.get_array<S>()->data.col[i]; r = (std::decay_t<decltype(r)>)v; }
#define AOS_STR(T, path) \
    [](Archetype& a, size_t i) -> const std::string& { return (*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, std::string v) { (*a.get_array<T>())[i].path = std::move(v); }

//  ВЫЧИСЛЯЕМОЕ поле — величина, которой в данных нет: только геттер, сеттер nullptr. Отдельного
//  флага не нужно, всё выводится из отсутствия сеттера — Save такую колонку не пишет (Load её
//  некуда положить), Load не читает, UI рисует меткой. Выражение видит ряд компонента как `c`
//  (у SoA — хранилище `c` и индекс `i`) и может звать методы, а не только читать члены.
#define AOS_CALC(T, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = (*a.get_array<T>())[i]; return (double)(expr); }, \
    nullptr
#define SOA_CALC(S, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = a.get_array<S>()->data; return (double)(expr); }, \
    nullptr
//  Строковый вариант: производная величина не обязана быть числом. Он же — единственный способ
//  показать то, чьё КОЛИЧЕСТВО лежит в данных: схема описывает тип компонента и переменного
//  числа полей выразить не может, а геттер — обычный код, он может всё.
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

    // Escape hatch: заданы — и генераторы по fields не работают вовсе.
    std::function<void(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*, ScenePool*)> custom_save;
    std::function<void(Archetype&, yyjson_val*, size_t, ScenePool*)>                       custom_load;

    // Память -> json: ВЕСЬ компонент архетипа (count строк) колонками по полям. pool == nullptr —
    // без словаря, строковые колонки пишутся и читаются именами.
    void Save(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp, ScenePool* pool) const;
    // json -> память: count дефолтных рядов, поверх них сеттеры по колонкам comp. Колонки короче
    // count оставляют дефолт, длиннее — усекаются; comp == nullptr даёт просто count дефолтных
    // рядов, этим путём и получает «пустой» компонент форма создания. ИНВАРИАНТ вызова: сущности
    // уже добавлены в arch.entities — строки дописываются в хвост колонок.
    void Load(Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool) const;
};

class ComponentSpecRegistry {
public:
    static ComponentSpecRegistry& Get();

    void Register(ComponentSpec s);
    const ComponentSpec* ByName(const std::string& name) const;   // для загрузки
    const ComponentSpec* ByType(std::type_index t) const;         // для сохранения
    const std::vector<ComponentSpec>& All() const { return specs_; }

private:
    std::vector<ComponentSpec>                    specs_;
    std::unordered_map<std::string, size_t>       by_name_;
    std::unordered_map<std::type_index, size_t>   by_type_;
};

// Спецификации встроенных компонентов; зовут один раз на старте движка. Идемпотентна.
void RegisterBuiltinComponentSpecs();
