#pragma once
#include <string>
#include <vector>
#include <map>
#include <typeindex>
#include <functional>
#include <unordered_map>
#include "ComponentStorage.h"
#include "CommandId.h"
#include "yyjson.h"

// Angle держит радианы и в данных, и в файле, градусы живут только в слайдере, поэтому lo/hi
// у него задают в ГРАДУСАХ.
enum class FieldKind : uint8_t { F32, U32, Bool, Str, AssetModel, Angle };

// Сколько ПОДРЯД идущих полей рисуются одним виджетом; ставится на первое поле группы. Явно, а не
// по именам колонок: у Transform x,y,z — это m00,m01,m02, а перенос лежит в w,d,h.
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
    const char* key = nullptr;
    FieldKind   kind = FieldKind::F32;

    double (*get_num)(Archetype&, size_t) = nullptr;
    void   (*set_num)(Archetype&, size_t, double) = nullptr;
    std::function<const std::string&(Archetype&, size_t)> get_str;
    std::function<void(Archetype&, size_t, std::string)>  set_str;

    float lo = 0, hi = 0;          // lo == hi → диапазон не задан
    float speed = 0.05f;
    bool  clamp_on_load = false;
    bool  ui_readonly = false;     // прямая запись порвала бы инварианты (Parent.parent)

    CommandId cmd = CommandId::None;

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

// Дефолты живут в member-инициализаторах самого компонента: недостающая в файле колонка их
// просто оставляет.
template<typename T>
void AddDefaultAoS(Archetype& arch) { arch.ensure_component<T>(); arch.get_array<T>()->add(T{}); }
template<typename SoA, typename Proxy>
void AddDefaultSoA(Archetype& arch) { arch.ensure_component<SoA>(); arch.get_array<SoA>()->add(Proxy{}); }


class ScenePool {
public:
    struct List {
        std::vector<std::string>                  names;
        std::unordered_map<std::string, uint32_t> index;
        uint32_t Intern(const std::string& name);
    };

    List& operator[](const std::string& list_name) { return lists_[list_name]; }
    List* Find(const std::string& list_name);

    const char* Cell(const List* list, yyjson_val* v);

    void Write(yyjson_mut_doc* doc, yyjson_mut_val* root) const;
    void Read(yyjson_val* root);

    uint32_t Misses() const { return misses_; }

private:
    std::map<std::string, List> lists_;
    uint32_t misses_ = 0;
};

constexpr const char* FieldPoolName(FieldKind k)
{
    return k == FieldKind::AssetModel ? "models" : nullptr;
}

#define AOS_NUM(T, path) \
    [](Archetype& a, size_t i) -> double { return (double)(*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, double v) { auto& r = (*a.get_array<T>())[i].path; r = (std::decay_t<decltype(r)>)v; }
#define SOA_NUM(S, col) \
    [](Archetype& a, size_t i) -> double { return (double)a.get_array<S>()->data.col[i]; }, \
    [](Archetype& a, size_t i, double v) { auto& r = a.get_array<S>()->data.col[i]; r = (std::decay_t<decltype(r)>)v; }
#define AOS_STR(T, path) \
    [](Archetype& a, size_t i) -> const std::string& { return (*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, std::string v) { (*a.get_array<T>())[i].path = std::move(v); }

//  ВЫЧИСЛЯЕМОЕ поле: сеттер nullptr, и по его отсутствию Save колонку не пишет, Load не читает,
//  UI рисует меткой. Выражение видит ряд компонента как `c` (у SoA — хранилище `c` и индекс `i`).
#define AOS_CALC(T, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = (*a.get_array<T>())[i]; return (double)(expr); }, \
    nullptr
#define SOA_CALC(S, expr) \
    [](Archetype& a, size_t i) -> double { auto& c = a.get_array<S>()->data; return (double)(expr); }, \
    nullptr
//  Он же единственный способ показать то, чьё КОЛИЧЕСТВО лежит в данных: схема описывает тип,
//  переменного числа полей она не выражает.
#define AOS_CALC_STR(T, expr) \
    [](Archetype& a, size_t i) -> const std::string& { \
        auto& c = (*a.get_array<T>())[i]; \
        thread_local std::string s; s = (expr); return s; }, \
    nullptr


struct ComponentSpec {
    std::string     name;
    std::type_index sig_type;

    void (*add_default)(Archetype&) = nullptr;
    std::vector<FieldSpec> fields;
    void (*after_edit)(Archetype&, size_t) = nullptr;

    // Заданы — генераторы по fields не работают вовсе.
    std::function<void(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*, ScenePool*)> custom_save;
    std::function<void(Archetype&, yyjson_val*, size_t, ScenePool*)>                       custom_load;

    void Save(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp, ScenePool* pool) const;
    void Load(Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool) const;
};

class ComponentSpecRegistry {
public:
    static ComponentSpecRegistry& Get();

    void Register(ComponentSpec s);
    const ComponentSpec* ByName(const std::string& name) const;
    const ComponentSpec* ByType(std::type_index t) const;
    const std::vector<ComponentSpec>& All() const { return specs_; }

private:
    std::vector<ComponentSpec>                    specs_;
    std::unordered_map<std::string, size_t>       by_name_;
    std::unordered_map<std::type_index, size_t>   by_type_;
};

void RegisterBuiltinComponentSpecs();
