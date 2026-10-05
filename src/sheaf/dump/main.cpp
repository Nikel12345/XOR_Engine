// SheafDump: печатает файл Sheaf текстом для git diff (docs/sheaf.md, «Diff в git»).
// Запуск: SheafDump файл.sheaf — текст в stdout, ошибка в stderr и код 1.
#include "Sheaf.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <new>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using sheaf::Column;
using sheaf::Type;

namespace {

struct Dumper {
    const sheaf::File&       file;
    std::vector<std::string> table_names;
    std::vector<uint32_t>    bases;
    std::string              out;

    void Str(const std::string& s)
    {
        out += '"';
        for (const char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
            else if (ch == '\n') out += "\\n";
            else if (ch == '\r') out += "\\r";
            else if (ch == '\t') out += "\\t";
            else if (u < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02X", u);
                out += buf;
            }
            else out += ch;
        }
        out += '"';
    }

    // Сквозной номер сдвигается от любой правки таблиц выше по файлу, поэтому ссылка печатается
    // таблицей и строкой в ней: в diff меняются только ссылки на правленую таблицу.
    void Ref(uint32_t global)
    {
        const auto it = std::upper_bound(bases.begin(), bases.end(), global);
        const size_t t = it - bases.begin();
        if (t == 0 || global - bases[t - 1] >= file.tables[t - 1].rows) {
            out += "?" + std::to_string(global);
            return;
        }
        out += "-> " + table_names[t - 1] + " " + std::to_string(global - bases[t - 1]);
    }

    void Value(const Column& c, size_t i)
    {
        if ((c.flags & sheaf::Nullable) && !c.present[i]) { out += "—"; return; }
        const uint32_t v = c.values[i];
        switch (c.type) {
        case Type::F32: {
            char buf[32];
            const auto res = std::to_chars(buf, buf + sizeof buf, std::bit_cast<float>(v));
            out.append(buf, res.ptr);
            break;
        }
        case Type::I32:  out += std::to_string(std::bit_cast<int32_t>(v)); break;
        case Type::Bool: out += v ? "1" : "0"; break;
        case Type::Str:  Str(file.strings[v]); break;
        case Type::Ref:  Ref(v); break;
        default:         out += std::to_string(v); break;
        }
    }

    void Run()
    {
        uint32_t base = 0;
        for (const sheaf::Table& t : file.tables) {
            std::string name;
            for (const sheaf::Component& c : t.components) name += (name.empty() ? "" : "+") + c.name;
            table_names.push_back(name.empty() ? "(пусто)" : name);
            bases.push_back(base);
            base += t.rows;
        }

        for (size_t ti = 0; ti < file.tables.size(); ++ti) {
            const sheaf::Table& t = file.tables[ti];
            std::vector<std::vector<size_t>> first;   // List: начало строки в values, по полю
            for (const sheaf::Component& comp : t.components)
                for (const Column& c : comp.fields) {
                    std::vector<size_t>& f = first.emplace_back();
                    if (!(c.flags & sheaf::List)) continue;
                    f.resize(t.rows);
                    size_t acc = 0;
                    for (uint32_t r = 0; r < t.rows; ++r) { f[r] = acc; acc += c.lengths[r]; }
                }

            for (uint32_t r = 0; r < t.rows; ++r) {
                out += table_names[ti] + " " + std::to_string(r) + "\n";
                size_t fi = 0;
                for (const sheaf::Component& comp : t.components)
                    for (const Column& c : comp.fields) {
                        out += "\t" + comp.name + "." + c.name + " = ";
                        if (c.flags & sheaf::List) {
                            out += '[';
                            for (size_t i = first[fi][r], e = i + c.lengths[r]; i < e; ++i) {
                                if (i != first[fi][r]) out += ", ";
                                Value(c, i);
                            }
                            out += ']';
                        } else {
                            Value(c, r);
                        }
                        out += '\n';
                        ++fi;
                    }
            }
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "SheafDump: нужен один аргумент — путь к .sheaf\n"); return 1; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "SheafDump: не открыть %s\n", argv[1]); return 1; }
    const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::expected<sheaf::File, std::string> file;
    try {
        file = sheaf::Read(bytes);
    } catch (const std::bad_alloc&) {
        file = std::unexpected(std::string("не хватило памяти: число объектов в файле, видимо, испорчено"));
    }
    if (!file) { std::fprintf(stderr, "SheafDump: %s: %s\n", argv[1], file.error().c_str()); return 1; }

    Dumper d{ *file };
    d.Run();
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   // иначе CRT допишет \r к каждой строке
#endif
    std::fwrite(d.out.data(), 1, d.out.size(), stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
