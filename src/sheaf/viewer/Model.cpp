#include "Model.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numeric>

using sheaf::Column;
using sheaf::Type;

namespace {

// Значение, с которым фильтр сравнивает ячейки.
struct Wanted {
    bool     absent  = false;   // ищем пустую ячейку nullable
    bool     nothing = false;   // строки нет в файле — совпадений заведомо нет
    uint32_t bits    = 0;
    float    f       = 0.0f;
};

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

template<class T>
bool ParseNumber(const std::string& s, T& out)
{
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && end == s.data() + s.size();
}

std::optional<Wanted> ParseWanted(const Column& c, const std::vector<std::string>& strings, const std::string& raw)
{
    const std::string s = Trim(raw);
    Wanted w;
    if ((c.flags & sheaf::Nullable) && (s == "-" || s == "—")) { w.absent = true; return w; }
    switch (c.type) {
    case Type::F32:
        if (!ParseNumber(s, w.f)) return std::nullopt;
        return w;
    case Type::I32: {
        int32_t v = 0;
        if (!ParseNumber(s, v)) return std::nullopt;
        w.bits = std::bit_cast<uint32_t>(v);
        return w;
    }
    case Type::Bool:
        if (s == "1" || s == "true")       w.bits = 1;
        else if (s == "0" || s == "false") w.bits = 0;
        else return std::nullopt;
        return w;
    case Type::Str: {
        const std::string text = s == "\"\"" ? std::string{} : s;
        const auto it = std::find(strings.begin(), strings.end(), text);
        if (it == strings.end()) w.nothing = true;
        else w.bits = sheaf::Narrow<uint32_t>(it - strings.begin());
        return w;
    }
    case Type::U8:
        if (!ParseNumber(s, w.bits) || w.bits > 0xFF) return std::nullopt;
        return w;
    default:
        if (!ParseNumber(s, w.bits)) return std::nullopt;
        return w;
    }
}

bool Equal(const Column& c, uint32_t v, const Wanted& w)
{
    if (c.type != Type::F32) return v == w.bits;
    const float f = std::bit_cast<float>(v);
    return f == w.f || (std::isnan(f) && std::isnan(w.f));
}

bool CellMatches(const Column& c, size_t i, const Wanted& w)
{
    const bool present = !(c.flags & sheaf::Nullable) || c.present[i];
    if (w.absent) return !present;
    return present && Equal(c, c.values[i], w);
}

bool RowMatches(const ViewColumn& vc, uint32_t r, const Wanted& w)
{
    const Column& c = *vc.col;
    if (!(c.flags & sheaf::List)) return CellMatches(c, r, w);
    for (size_t i = vc.first[r], e = i + c.lengths[r]; i < e; ++i)
        if (CellMatches(c, i, w)) return true;
    return false;
}

// Порядок ключей совпадает с порядком значений: float — с учётом знака, отсутствующее — раньше всех.
uint64_t SortKey(const Column& c, uint32_t r, const std::vector<uint32_t>& str_rank)
{
    if ((c.flags & sheaf::Nullable) && !c.present[r]) return 0;
    const uint32_t v = c.values[r];
    uint32_t k = v;
    switch (c.type) {
    case Type::F32: k = (v & 0x80000000u) ? ~v : (v | 0x80000000u); break;
    case Type::I32: k = v ^ 0x80000000u; break;
    case Type::Str: k = str_rank[v]; break;
    default: break;
    }
    return (uint64_t{ 1 } << 32) | k;
}

} // namespace

bool Model::Open(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "не открыть файл " + path; return false; }
    const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    auto read = sheaf::Read(bytes);
    if (!read) { error = path + ": " + read.error(); return false; }

    file_  = std::move(*read);
    path_  = path;
    bytes_ = bytes.size();
    str_rank_.clear();
    tables_.clear();
    total_rows_ = 0;
    for (const sheaf::Table& t : file_->tables) {
        ViewTable& vt = tables_.emplace_back();
        vt.table = &t;
        vt.base  = total_rows_;
        vt.hidden.assign(t.components.size(), false);
        for (uint32_t ci = 0; ci < t.components.size(); ++ci)
            for (const Column& c : t.components[ci].fields) {
                ViewColumn& vc = vt.columns.emplace_back();
                vc.col       = &c;
                vc.component = ci;
                if (c.flags & sheaf::List) {
                    vc.first.resize(t.rows);
                    size_t acc = 0;
                    for (uint32_t r = 0; r < t.rows; ++r) { vc.first[r] = acc; acc += c.lengths[r]; }
                }
            }
        Rebuild(vt);
        total_rows_ += t.rows;
    }
    return true;
}

std::optional<std::pair<uint32_t, uint32_t>> Model::Locate(uint32_t global) const
{
    const auto it = std::upper_bound(tables_.begin(), tables_.end(), global,
                                     [](uint32_t g, const ViewTable& t) { return g < t.base; });
    if (it == tables_.begin()) return std::nullopt;
    const ViewTable& t = *std::prev(it);
    if (global - t.base >= t.table->rows) return std::nullopt;
    return std::pair{ sheaf::Narrow<uint32_t>(std::prev(it) - tables_.begin()), global - t.base };
}

bool Model::Rebuild(ViewTable& t)
{
    const uint32_t rows = t.table->rows;
    std::vector<uint32_t> view;
    if (t.filter.column >= 0) {
        const ViewColumn& vc = t.columns[t.filter.column];
        const std::optional<Wanted> w = ParseWanted(*vc.col, file_->strings, t.filter.text);
        if (!w) return false;
        if (!w->nothing)
            for (uint32_t r = 0; r < rows; ++r)
                if (RowMatches(vc, r, *w)) view.push_back(r);
    } else {
        view.resize(rows);
        std::iota(view.begin(), view.end(), 0u);
    }

    const Column* sort_col = t.sort_column >= 0 ? t.columns[t.sort_column].col : nullptr;
    if (sort_col && !(sort_col->flags & sheaf::List)) {
        if (sort_col->type == Type::Str && str_rank_.empty()) {
            const std::vector<std::string>& s = file_->strings;
            std::vector<uint32_t> ids(s.size());
            std::iota(ids.begin(), ids.end(), 0u);
            std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return s[a] < s[b]; });
            str_rank_.resize(s.size());
            for (uint32_t i = 0; i < ids.size(); ++i) str_rank_[ids[i]] = i;
        }
        std::vector<uint64_t> key(rows);
        for (uint32_t r : view) key[r] = SortKey(*sort_col, r, str_rank_);
        if (t.sort_desc) std::stable_sort(view.begin(), view.end(), [&](uint32_t a, uint32_t b) { return key[a] > key[b]; });
        else             std::stable_sort(view.begin(), view.end(), [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
    } else if (t.sort_desc) {
        std::reverse(view.begin(), view.end());
    }

    t.view    = std::move(view);
    t.matched = sheaf::Narrow<uint32_t>(t.view.size());
    return true;
}

std::string Model::FormatValue(const Column& c, uint32_t v) const
{
    switch (c.type) {
    case Type::F32: {
        char buf[32];
        const auto res = std::to_chars(buf, buf + sizeof buf, std::bit_cast<float>(v));
        return std::string(buf, res.ptr);
    }
    case Type::I32:  return std::to_string(std::bit_cast<int32_t>(v));
    case Type::Bool: return v ? "1" : "0";
    case Type::Str: {
        const std::string& s = file_->strings[v];
        return s.empty() ? "\"\"" : s;
    }
    default:         return std::to_string(v);
    }
}

std::string Model::FormatCell(const ViewColumn& vc, uint32_t row) const
{
    const Column& c = *vc.col;
    if (c.flags & sheaf::List) return "[" + std::to_string(c.lengths[row]) + "]";
    if ((c.flags & sheaf::Nullable) && !c.present[row]) return "—";
    return FormatValue(c, c.values[row]);
}

const char* TypeName(Type t)
{
    switch (t) {
    case Type::F32:  return "f32";
    case Type::U32:  return "u32";
    case Type::I32:  return "i32";
    case Type::U8:   return "u8";
    case Type::Bool: return "bool";
    case Type::Str:  return "str";
    case Type::Ref:  return "ref";
    }
    return "?";
}

const char* EncodingName(sheaf::Encoding e)
{
    switch (e) {
    case sheaf::Encoding::Raw:      return "подряд";
    case sheaf::Encoding::Const:    return "одно на всех";
    case sheaf::Encoding::Defaults: return "отметки";
    case sheaf::Encoding::Dict:     return "уникальные";
    }
    return "?";
}
