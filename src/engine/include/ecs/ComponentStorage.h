#pragma once
// Машинерия хранилища ECS: тип-стёртые колонки архетипа и трейты. Компоненты лежат в
// BaseComponents.h отдельно ради времени сборки: ObjectManager.h видит почти весь движок,
// и правка поля компонента иначе пересобирала бы всё, а не только знающие о нём TU.
//
// Инклуды самодостаточны намеренно: заголовок включают и PCH-free либы (Physics).
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <tuple>
#include <utility>
#include <type_traits>
#include <cmath>
#include <SDL3/SDL.h>

struct TextureData;

using f_restrict_pointer = float const* __restrict;
using Entity = uint32_t;

// SoA-компонент и его прокси — ОДИН компонент в двух видах. Хранилище (тег soa_tag) — это
// структура параллельных колонок-векторов, она и лежит в архетипе; прокси (тег related_soa) —
// одна строка полями, и только им компонент передают в CreateEntity. Отсюда весь путь:
//   сигнатура архетипа считается по related_soa, то есть по типу ХРАНИЛИЩА, а не прокси;
//   ComponentArray<SoA> держит одно поле data (колонки), а не вектор элементов — отдельного
//     элемента в памяти нет вообще;
//   add(proxy) уходит в proxy.emplace_to(storage): по одному push_back в каждую колонку;
//   swap_remove идёт по columns() — все колонки обязаны оставаться выровненными по индексу,
//     иначе строки сущностей разъедутся.
// Поэтому же ForEach/GetComponent отдают SoAElement: ссылаться не на что, возвращается пара
// (хранилище, индекс). Компонент становится SoA, унаследовав SoAProxyAddable<себя> и объявив
// size()/columns(); прокси — объявив related_soa и emplace_to.
template<typename, typename = void>
struct is_soa : std::false_type {};

template<typename T>
struct is_soa<T, std::void_t<typename T::soa_tag>> : std::true_type {};
template<typename, typename = void>
struct has_related_soa : std::false_type {};

template<typename T>
struct has_related_soa<T, std::void_t<typename T::related_soa>> : std::true_type {};

// Ёмкость под n строк, но не меньше удвоенной: точный reserve(size + 1) при добавлении по одной
// строке (форма создания идёт тем же LoadScene) перевыделял бы вектор на каждой.
template<typename V>
void ReserveRows(V& v, size_t n)
{
    if (n > v.capacity()) v.reserve(std::max(n, v.capacity() * 2));
}

template<typename Derived>
struct SoAProxyAddable {
    template<typename Proxy>
    auto add(const Proxy& proxy)
        -> decltype(proxy.emplace_to(static_cast<Derived&>(*this)), void()) {
        proxy.emplace_to(static_cast<Derived&>(*this));
    }

    void reserve(size_t n) {
        std::apply([&](auto&... col) { (ReserveRows(col, n), ...); }, static_cast<Derived&>(*this).columns());
    }

    void repeat_last(size_t n) {
        std::apply([&](auto&... col) {
            (..., ([&] {
                const auto last = col.back();
                col.insert(col.end(), n, last);
                }()));
            }, static_cast<Derived&>(*this).columns());
    }

    void swap_remove(size_t i) {
        std::apply([&](auto&... col) {
            (..., ([&] {
                const size_t last = col.size() - 1;
                if (i != last) col[i] = std::move(col[last]);
                col.pop_back();
                }()));
            }, static_cast<Derived&>(*this).columns());
    }
};

struct IComponentArray {
    virtual ~IComponentArray() = default;
    virtual void swap_remove(size_t i) = 0;
    virtual void reserve(size_t rows) = 0;
    // Дописывает n копий последней строки: так одна дефолтная строка становится n + 1.
    virtual void repeat_last(size_t n) = 0;
};

template<typename T, typename = void>
struct ComponentArray : IComponentArray {
    std::vector<T> data;

    void add(const T& v) { data.push_back(v); }
    T& operator[](size_t i) { return data[i]; }
    size_t size() const { return data.size(); }
    void reserve(size_t rows) override { ReserveRows(data, rows); }
    void repeat_last(size_t n) override
    {
        const T last = data.back();
        data.insert(data.end(), n, last);
    }

    void swap_remove(size_t i) override {
        const size_t last = data.size() - 1;
        if (i != last) data[i] = std::move(data[last]);
        data.pop_back();
    };
};

template<typename T>
struct ComponentArray<T, std::enable_if_t<is_soa<T>::value>> : IComponentArray {
    T data;
    template<typename Proxy>
    auto add(const Proxy& proxy) -> decltype(data.add(proxy), void()) { data.add(proxy); }
    size_t size() const { return data.size(); }
    void reserve(size_t rows) override { data.reserve(rows); }
    void repeat_last(size_t n) override { data.repeat_last(n); }

    void swap_remove(size_t i) override { data.swap_remove(i); }
};


struct Archetype {
    std::vector<Entity> entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentArray>> components;
    // Номер первой инстанс-строки архетипа в рендер-буферах: строка сущности =
    // render_instance_base + её индекс в колонках. Считает BatchBuilder::RecalculateInstanceOffsets
    // (только для архетипов с Draw+Positions, в порядке обхода scene->archetypes), читают
    // дата-модули. Само ECS его не трогает — поле живёт здесь как ячейка для рендера.
    uint32_t render_instance_base = 0;

    template<typename T>
    ComponentArray<T>* get_array() {
        auto it = components.find(std::type_index(typeid(T)));
        if (it == components.end()) {
            return nullptr;
        };
        return static_cast<ComponentArray<T>*>(it->second.get());
    }

    template<typename T>
    void ensure_component() {
        auto idx = std::type_index(typeid(T));
        if (!components.count(idx))
            components[idx] = std::make_unique<ComponentArray<T>>();
    }
    void swap_remove(size_t i) {
        for (auto& [type, arr] : components)
            arr->swap_remove(i);
    }
    // Только entities и уже заведённые колонки: массив компонента появляется на первом add.
    void reserve(size_t rows) {
        ReserveRows(entities, rows);
        for (auto& [type, arr] : components)
            arr->reserve(rows);
    }
};

