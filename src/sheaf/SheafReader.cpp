#include "Sheaf.h"
#include <bit>
#include <cstring>
#include <utility>

namespace sheaf {

namespace {

// Значения строки r источника src: values[first[r] .. first[r] + длина).
std::vector<size_t> RowStarts(const Column& src)
{
    std::vector<size_t> first;
    if (!(src.flags & List)) return first;
    first.resize(src.lengths.size());
    size_t acc = 0;
    for (size_t r = 0; r < first.size(); ++r) { first[r] = acc; acc += src.lengths[r]; }
    return first;
}

void AppendRow(Column& dst, const Column& src, const std::vector<size_t>& first, uint32_t r)
{
    size_t b = r, e = size_t{ r } + 1;
    if (src.flags & List) {
        b = first[r];
        e = b + src.lengths[r];
        dst.lengths.push_back(src.lengths[r]);
    }
    dst.values.insert(dst.values.end(), src.values.begin() + b, src.values.begin() + e);
    if (src.flags & Nullable)
        dst.present.insert(dst.present.end(), src.present.begin() + b, src.present.begin() + e);
}

// Каждая функция чтения возвращает false после Fail: дальше файл не разбирается.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes, TableVisitor* visitor = nullptr) : buf_(bytes), visitor_(visitor) {}

    std::expected<File, std::string> Run()
    {
        if (!ReadFile()) return std::unexpected(std::move(error_));
        return std::move(file_);
    }

private:
    std::span<const uint8_t> buf_;
    TableVisitor*            visitor_;
    size_t                   pos_ = 0;
    std::string              where_;
    std::string              error_;
    File                     file_;

    size_t Left() const { return buf_.size() - pos_; }

    bool Fail(const std::string& msg)
    {
        error_ = where_.empty() ? msg : where_ + ": " + msg;
        return false;
    }

    bool Need(size_t bytes) { return bytes <= Left() || Fail("файл обрывается"); }

    // Счётчик, за которым идут count записей не короче min_bytes: мусорное число отсекается до
    // аллокации под него.
    bool NeedCount(size_t count, size_t min_bytes) { return count <= Left() / min_bytes || Fail("файл обрывается"); }

    uint8_t  U8()  { return buf_[pos_++]; }
    uint32_t U32()
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{ buf_[pos_ + i] } << (8 * i);
        pos_ += 4;
        return v;
    }
    uint32_t Value(Type t) { return ValueSize(t) == 1 ? U8() : U32(); }

    bool CheckStr(uint32_t id)
    {
        return id < file_.strings.size()
            || Fail("номер строки " + std::to_string(id) + " за пределами массива строк ("
                    + std::to_string(file_.strings.size()) + ")");
    }

    bool ReadFile()
    {
        if (!Need(4) || std::memcmp(buf_.data(), "SHEF", 4) != 0) return Fail("нет сигнатуры SHEF");
        pos_ = 4;

        if (!Need(4)) return false;
        const uint32_t nstr = U32();
        if (!NeedCount(nstr, 4)) return false;
        file_.strings.resize(nstr);
        for (std::string& s : file_.strings) {
            if (!Need(4)) return false;
            const uint32_t len = U32();
            if (!Need(len)) return false;
            s.assign(reinterpret_cast<const char*>(buf_.data() + pos_), len);
            pos_ += len;
        }

        if (!Need(4)) return false;
        const uint32_t ntab = U32();
        if (!NeedCount(ntab, 8)) return false;
        if (visitor_) {
            for (uint32_t t = 0; t < ntab; ++t) {
                Table table;
                if (!ReadTable(t, table)) return false;
            }
        } else {
            file_.tables.resize(ntab);
            for (uint32_t t = 0; t < ntab; ++t)
                if (!ReadTable(t, file_.tables[t])) return false;
        }
        where_.clear();
        return Left() == 0 || Fail("после последней таблицы лишние байты: " + std::to_string(Left()));
    }

    bool ReadTable(uint32_t index, Table& table)
    {
        where_ = "таблица " + std::to_string(index);
        if (!Need(8)) return false;
        table.rows = U32();
        const uint32_t ncomp = U32();
        if (!NeedCount(ncomp, 8)) return false;
        table.components.resize(ncomp);
        for (Component& comp : table.components) {
            if (!Need(8)) return false;
            const uint32_t name = U32();
            if (!CheckStr(name)) return false;
            comp.name = file_.strings[name];
            const uint32_t nfield = U32();
            if (!NeedCount(nfield, 7)) return false;
            comp.fields.resize(nfield);
            for (Column& col : comp.fields)
                if (!ReadFieldHeader(comp.name, col)) return false;
        }

        std::vector<Destination> dests;
        if (visitor_) {
            size_t fields = 0;
            for (const Component& comp : table.components) fields += comp.fields.size();
            dests.resize(fields);
            visitor_->Begin(table, file_.strings, dests);
        }

        const std::string table_where = where_;
        size_t k = 0;
        for (Component& comp : table.components)
            for (Column& col : comp.fields) {
                where_ = table_where + ", " + comp.name + "." + col.name;
                const size_t start = pos_;
                const bool direct = visitor_ && dests[k].first && !col.flags
                                 && ValueSize(col.type) == 4 && col.type != Type::Str;
                if (direct) col.direct = true;
                if (!(direct ? ReadFieldInto(col, table.rows, dests[k]) : ReadFieldData(col, table.rows))) return false;
                col.encoded_bytes = pos_ - start;
                ++k;
            }
        if (visitor_) visitor_->End(table, file_.strings);
        return true;
    }

    // Поле без флагов с 4-байтным числом: «все подряд» копируется из файла как есть, остальные
    // способы пишут по значению на строку.
    bool ReadFieldInto(const Column& col, uint32_t rows, const Destination& dst)
    {
        static_assert(std::endian::native == std::endian::little, "значения копируются из файла без перестановки байт");
        auto put = [&](uint32_t r, uint32_t v) { std::memcpy(dst.first + size_t{ r } * dst.stride, &v, sizeof v); };
        switch (col.encoding) {
        case Encoding::Raw: {
            if (!NeedCount(rows, 4)) return false;
            const uint8_t* src = buf_.data() + pos_;
            if (dst.stride == 4) std::memcpy(dst.first, src, size_t{ rows } * 4);
            else for (uint32_t r = 0; r < rows; ++r) std::memcpy(dst.first + size_t{ r } * dst.stride, src + size_t{ r } * 4, 4);
            pos_ += size_t{ rows } * 4;
            return true;
        }
        case Encoding::Const: {
            if (!Need(4)) return false;
            const uint32_t v = U32();
            for (uint32_t r = 0; r < rows; ++r) put(r, v);
            return true;
        }
        case Encoding::Defaults: {
            const size_t bytes = rows / 8 + (rows % 8 != 0);
            if (!Need(bytes)) return false;
            const size_t marks = pos_;
            size_t own = 0;
            for (uint32_t r = 0; r < rows; ++r) own += (buf_[marks + r / 8] >> (r % 8)) & 1;
            pos_ += bytes;
            if (!NeedCount(own, 4)) return false;
            for (uint32_t r = 0; r < rows; ++r)
                put(r, ((buf_[marks + r / 8] >> (r % 8)) & 1) ? U32() : col.def);
            return true;
        }
        case Encoding::Dict: {
            if (!Need(5)) return false;
            const uint32_t k     = U32();
            const uint8_t  width = U8();
            if (width != 1 && width != 2 && width != 4) return Fail("ширина номера " + std::to_string(width) + ", а не 1, 2 или 4");
            if (!NeedCount(k, 4)) return false;
            std::vector<uint32_t> uniq(k);
            std::memcpy(uniq.data(), buf_.data() + pos_, size_t{ k } * 4);
            pos_ += size_t{ k } * 4;
            if (!NeedCount(rows, width)) return false;
            for (uint32_t r = 0; r < rows; ++r) {
                uint32_t id = 0;
                for (uint32_t b = 0; b < width; ++b) id |= uint32_t{ U8() } << (8 * b);
                if (id >= k) return Fail("номер " + std::to_string(id) + " в таблице уникальных не меньше K = " + std::to_string(k));
                put(r, uniq[id]);
            }
            return true;
        }
        }
        return Fail("неизвестный способ записи");
    }

    bool ReadFieldHeader(const std::string& comp_name, Column& col)
    {
        if (!Need(7)) return false;
        const uint32_t name = U32();
        if (!CheckStr(name)) return false;
        col.name = file_.strings[name];
        const std::string table_where = where_;
        where_ += ", " + comp_name + "." + col.name;

        const uint8_t type = U8(), flags = U8(), enc = U8();
        if (type > std::to_underlying(Type::Ref))       return Fail("неизвестный тип " + std::to_string(type));
        if (flags & ~(List | Nullable))                  return Fail("заняты свободные биты флагов: " + std::to_string(flags));
        if (enc > std::to_underlying(Encoding::Dict))    return Fail("неизвестный способ записи " + std::to_string(enc));
        col.type     = Type{ type };
        col.flags    = flags;
        col.encoding = Encoding{ enc };
        if (col.encoding == Encoding::Defaults && flags) return Fail("способ 2 (отметки) у поля с флагами");

        if (!(flags & List)) {
            if (!Need(ValueSize(col.type))) return false;
            col.def = Value(col.type);
            if (col.type == Type::Str && !CheckStr(col.def)) return false;
        }
        where_ = table_where;
        return true;
    }

    // Способ 0 для n строк в out (тип и флаги уже стоят).
    bool ReadRaw(Column& out, uint32_t n)
    {
        size_t cells = n;
        if (out.flags & List) {
            if (!NeedCount(n, 4)) return false;
            out.lengths.resize(n);
            cells = 0;
            for (uint32_t& len : out.lengths) { len = U32(); cells += len; }
        }
        size_t stored = cells;
        if (out.flags & Nullable) {
            const size_t bytes = cells / 8 + (cells % 8 != 0);
            if (!Need(bytes)) return false;
            out.present.resize(cells);
            stored = 0;
            for (size_t i = 0; i < cells; ++i) {
                out.present[i] = ((buf_[pos_ + i / 8] >> (i % 8)) & 1) != 0;
                stored += out.present[i];
            }
            pos_ += bytes;
        }
        if (!NeedCount(stored, ValueSize(out.type))) return false;
        out.values.assign(cells, 0);
        for (size_t i = 0; i < cells; ++i) {
            if ((out.flags & Nullable) && !out.present[i]) continue;
            out.values[i] = Value(out.type);
            if (out.type == Type::Str && !CheckStr(out.values[i])) return false;
        }
        return true;
    }

    bool ReadFieldData(Column& col, uint32_t rows)
    {
        switch (col.encoding) {
        case Encoding::Raw:
            return ReadRaw(col, rows);

        case Encoding::Const: {
            Column one{ .type = col.type, .flags = col.flags };
            if (!ReadRaw(one, 1)) return false;
            const std::vector<size_t> first = RowStarts(one);
            col.values.reserve(size_t{ rows } * one.values.size());
            col.present.reserve(size_t{ rows } * one.present.size());
            col.lengths.reserve(one.lengths.empty() ? 0 : rows);
            for (uint32_t r = 0; r < rows; ++r) AppendRow(col, one, first, 0);
            return true;
        }

        case Encoding::Defaults: {
            const size_t bytes = rows / 8 + (rows % 8 != 0);
            if (!Need(bytes)) return false;
            const size_t marks = pos_;
            size_t own = 0;
            for (uint32_t r = 0; r < rows; ++r) own += (buf_[marks + r / 8] >> (r % 8)) & 1;
            pos_ += bytes;
            if (!NeedCount(own, ValueSize(col.type))) return false;
            col.values.assign(rows, col.def);
            for (uint32_t r = 0; r < rows; ++r) {
                if (!((buf_[marks + r / 8] >> (r % 8)) & 1)) continue;
                col.values[r] = Value(col.type);
                if (col.type == Type::Str && !CheckStr(col.values[r])) return false;
            }
            return true;
        }

        case Encoding::Dict: {
            if (!Need(5)) return false;
            const uint32_t k     = U32();
            const uint8_t  width = U8();
            if (width != 1 && width != 2 && width != 4) return Fail("ширина номера " + std::to_string(width) + ", а не 1, 2 или 4");
            Column uniq{ .type = col.type, .flags = col.flags };
            if (!ReadRaw(uniq, k)) return false;
            if (!NeedCount(rows, width)) return false;
            const std::vector<size_t> first = RowStarts(uniq);
            if (col.flags & List) col.lengths.reserve(rows);
            else {
                col.values.reserve(rows);
                if (col.flags & Nullable) col.present.reserve(rows);
            }
            for (uint32_t r = 0; r < rows; ++r) {
                uint32_t id = 0;
                for (uint32_t b = 0; b < width; ++b) id |= uint32_t{ U8() } << (8 * b);
                if (id >= k) return Fail("номер " + std::to_string(id) + " в таблице уникальных не меньше K = " + std::to_string(k));
                AppendRow(col, uniq, first, id);
            }
            return true;
        }
        }
        return Fail("неизвестный способ записи");
    }
};

} // namespace

std::expected<File, std::string> Read(std::span<const uint8_t> bytes) { return Reader(bytes).Run(); }

std::expected<void, std::string> Read(std::span<const uint8_t> bytes, TableVisitor& visitor)
{
    auto file = Reader(bytes, &visitor).Run();
    if (!file) return std::unexpected(std::move(file.error()));
    return {};
}

} // namespace sheaf
