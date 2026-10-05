#include "Sheaf.h"
#include <bit>
#include <utility>

namespace sheaf {

uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }

uint32_t Writer::Intern(std::string_view s)
{
    auto it = index_.find(s);
    if (it != index_.end()) return it->second;
    const uint32_t id = Narrow<uint32_t>(strings_.size());
    strings_.emplace_back(s);
    index_.emplace(strings_.back(), id);
    return id;
}

void Writer::Add(Table table) { tables_.push_back(std::move(table)); }

namespace {

using Bytes = std::vector<uint8_t>;

// Таблица уникальных из чисел перестаёт окупаться, когда номер шире 2 байт: номер на строку весит
// столько же, сколько само значение. Дальше её не строим — на колонке уникальных float это
// миллион вставок в хэш впустую.
constexpr size_t kDictScalarLimit = 0x10000;

void PutU8(Bytes& out, uint8_t v) { out.push_back(v); }

void PutU32(Bytes& out, uint32_t v)
{
    for (uint32_t i = 0; i < 4; ++i) out.push_back(Narrow<uint8_t>((v >> (8 * i)) & 0xFFu));
}

void PutValue(Bytes& out, Type t, uint32_t v)
{
    if (ValueSize(t) == 1) PutU8(out, Narrow<uint8_t>(v));
    else                   PutU32(out, v);
}

void SetBit(Bytes& out, size_t base, size_t bit)
{
    out[base + bit / 8] |= Narrow<uint8_t>(1u << (bit % 8));
}

// Значения строки r лежат в values[First(r) .. First(r) + Count(r)).
struct Rows {
    const Column&         col;
    std::vector<uint32_t> first;

    explicit Rows(const Column& c) : col(c)
    {
        if (!(c.flags & List)) return;
        first.resize(c.lengths.size());
        uint32_t acc = 0;
        for (size_t r = 0; r < c.lengths.size(); ++r) { first[r] = acc; acc += c.lengths[r]; }
    }
    uint32_t First(uint32_t r) const { return (col.flags & List) ? first[r] : r; }
    uint32_t Count(uint32_t r) const { return (col.flags & List) ? col.lengths[r] : 1; }
    bool     Present(uint32_t i) const { return !(col.flags & Nullable) || col.present[i]; }
};

// Способ 0 для n строк, row_at(k) — номер k-й из них. Им же пишутся одна строка способа 1 и
// уникальные значения способа 3.
template<class RowAt>
void WriteRaw(Bytes& out, const Rows& rs, size_t n, RowAt row_at)
{
    const Column& c = rs.col;
    if (c.flags & List)
        for (size_t k = 0; k < n; ++k) PutU32(out, c.lengths[row_at(k)]);
    if (c.flags & Nullable) {
        size_t cells = 0;
        for (size_t k = 0; k < n; ++k) cells += rs.Count(row_at(k));
        const size_t base = out.size();
        out.resize(base + (cells + 7) / 8, 0);
        size_t bit = 0;
        for (size_t k = 0; k < n; ++k) {
            const uint32_t r = row_at(k);
            for (uint32_t i = rs.First(r), e = i + rs.Count(r); i < e; ++i, ++bit)
                if (c.present[i]) SetBit(out, base, bit);
        }
    }
    for (size_t k = 0; k < n; ++k) {
        const uint32_t r = row_at(k);
        for (uint32_t i = rs.First(r), e = i + rs.Count(r); i < e; ++i)
            if (rs.Present(i)) PutValue(out, c.type, c.values[i]);
    }
}

std::string RowKey(const Rows& rs, uint32_t r)
{
    std::string key;
    const uint32_t n = rs.Count(r);
    key.append(reinterpret_cast<const char*>(&n), sizeof n);
    for (uint32_t i = rs.First(r), e = i + n; i < e; ++i) {
        const bool p = rs.Present(i);
        key.push_back(p ? 1 : 0);
        if (p) key.append(reinterpret_cast<const char*>(&rs.col.values[i]), sizeof(uint32_t));
    }
    return key;
}

struct Encoded {
    Encoding enc;
    Bytes    data;
};

// Способы 1 и 3: оба держатся на одном разбиении строк на различные значения.
template<class Key, class KeyOf>
void EncodeByUniques(const Rows& rs, uint32_t rows, size_t limit, KeyOf key_of, std::vector<Encoded>& out)
{
    std::unordered_map<Key, uint32_t> index;
    std::vector<uint32_t> uniq;               // строка-представитель каждого различного значения
    std::vector<uint32_t> ids(rows);
    for (uint32_t r = 0; r < rows; ++r) {
        auto [it, inserted] = index.try_emplace(key_of(r), Narrow<uint32_t>(uniq.size()));
        if (inserted) {
            if (uniq.size() >= limit) return;
            uniq.push_back(r);
        }
        ids[r] = it->second;
    }
    if (uniq.size() == 1) {
        Encoded e{ Encoding::Const, {} };
        WriteRaw(e.data, rs, 1, [&](size_t) { return uniq[0]; });
        out.push_back(std::move(e));
    }
    if (uniq.size() == rows) return;
    const uint8_t width = uniq.size() <= 0x100 ? 1 : uniq.size() <= 0x10000 ? 2 : 4;
    Encoded e{ Encoding::Dict, {} };
    PutU32(e.data, Narrow<uint32_t>(uniq.size()));
    PutU8(e.data, width);
    WriteRaw(e.data, rs, uniq.size(), [&](size_t k) { return uniq[k]; });
    for (uint32_t id : ids)
        for (uint32_t b = 0; b < width; ++b) PutU8(e.data, Narrow<uint8_t>((id >> (8 * b)) & 0xFFu));
    out.push_back(std::move(e));
}

Encoded EncodeColumn(const Column& c, uint32_t rows)
{
    const bool list = c.flags & List;
    assert(list ? c.lengths.size() == rows : c.values.size() == rows);
    assert(!(c.flags & Nullable) || c.present.size() == c.values.size());

    const Rows rs(c);
    std::vector<Encoded> cands;
    cands.push_back({ Encoding::Raw, {} });
    WriteRaw(cands.back().data, rs, rows, [](size_t k) { return Narrow<uint32_t>(k); });

    if (rows > 0) {
        if (!list && !(c.flags & Nullable))
            EncodeByUniques<uint32_t>(rs, rows, kDictScalarLimit, [&](uint32_t r) { return c.values[r]; }, cands);
        else
            EncodeByUniques<std::string>(rs, rows, rows, [&](uint32_t r) { return RowKey(rs, r); }, cands);
    }

    if (!c.flags) {
        Encoded e{ Encoding::Defaults, Bytes((rows + 7) / 8, 0) };
        for (uint32_t r = 0; r < rows; ++r)
            if (c.values[r] != c.def) SetBit(e.data, 0, r);
        for (uint32_t r = 0; r < rows; ++r)
            if (c.values[r] != c.def) PutValue(e.data, c.type, c.values[r]);
        cands.push_back(std::move(e));
    }

    size_t best = 0;
    for (size_t i = 1; i < cands.size(); ++i)
        if (cands[i].data.size() < cands[best].data.size()) best = i;
    return std::move(cands[best]);
}

} // namespace

std::vector<uint8_t> Writer::Finish()
{
    Bytes body;
    PutU32(body, Narrow<uint32_t>(tables_.size()));
    for (const Table& t : tables_) {
        // Способ записи стоит в заголовке, а заголовок — перед данными: сначала кодируем всё.
        std::vector<Encoded> enc;
        for (const Component& comp : t.components)
            for (const Column& col : comp.fields) enc.push_back(EncodeColumn(col, t.rows));

        PutU32(body, t.rows);
        PutU32(body, Narrow<uint32_t>(t.components.size()));
        size_t k = 0;
        for (const Component& comp : t.components) {
            PutU32(body, Intern(comp.name));
            PutU32(body, Narrow<uint32_t>(comp.fields.size()));
            for (const Column& col : comp.fields) {
                PutU32(body, Intern(col.name));
                PutU8(body, std::to_underlying(col.type));
                PutU8(body, col.flags);
                PutU8(body, std::to_underlying(enc[k++].enc));
                if (!(col.flags & List)) PutValue(body, col.type, col.def);
            }
        }
        for (const Encoded& e : enc) body.insert(body.end(), e.data.begin(), e.data.end());
    }

    Bytes out = { 'S', 'H', 'E', 'F' };
    PutU32(out, Narrow<uint32_t>(strings_.size()));
    for (const std::string& s : strings_) {
        PutU32(out, Narrow<uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    out.insert(out.end(), body.begin(), body.end());
    tables_.clear();
    return out;
}

} // namespace sheaf
