#pragma once
// Формат Sheaf (docs/sheaf.md). Ни ECS, ни движка не знает: писатель принимает таблицы колонками и
// сам выбирает способ записи каждого поля, читатель возвращает их в том же виде.
#include <cassert>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sheaf {

enum class Type : uint8_t { F32, U32, I32, U8, Bool, Str, Ref };
enum Flag : uint8_t { List = 1, Nullable = 2 };
enum class Encoding : uint8_t { Raw, Const, Defaults, Dict };

// Свой аналог safe_* из Utils.h движка: библиотека формата движка не видит.
template<class To, class From>
To Narrow(From v)
{
    assert(std::in_range<To>(v));
    return static_cast<To>(v);
}

inline size_t ValueSize(Type t) { return (t == Type::U8 || t == Type::Bool) ? 1 : 4; }

// Значение любого типа хранится своими 32 битами (f32 — побитово, str — номер строки), поэтому
// одинаковыми способы записи считают только побитово равные значения: -0.0 не склеится с 0.0.
struct Column {
    std::string           name;
    Type                  type  = Type::F32;
    uint8_t               flags = 0;
    uint32_t              def   = 0;      // у списка не пишется
    std::vector<uint32_t> values;         // все значения подряд, у nullable отсутствующие тоже занимают ячейку
    std::vector<uint32_t> lengths;        // List: длина на строку
    std::vector<uint8_t>  present;        // Nullable: 1/0 на каждую ячейку values

    // Как поле лежало в файле: заполняет Read, Writer не читает.
    Encoding              encoding      = Encoding::Raw;
    size_t                encoded_bytes = 0;
};

struct Component {
    std::string         name;
    std::vector<Column> fields;
};

struct Table {
    uint32_t               rows = 0;
    std::vector<Component> components;
};

uint32_t Bits(float f);

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

class Writer {
public:
    uint32_t Intern(std::string_view s);
    void     Add(Table table);
    std::vector<uint8_t> Finish();

private:
    std::vector<std::string>                  strings_;
    std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> index_;
    std::vector<Table>                        tables_;
};

// Значения str в колонках — номера в strings.
struct File {
    std::vector<std::string> strings;
    std::vector<Table>       tables;
};

// Ошибка — текст с номером таблицы и именем поля. Колонки раскрыты: по значению (списку) на строку,
// отсутствующая ячейка nullable хранит 0.
std::expected<File, std::string> Read(std::span<const uint8_t> bytes);

} // namespace sheaf
