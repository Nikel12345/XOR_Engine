#pragma once
// Открытый файл: результат sheaf::Read и то, что дорого считать каждый кадр (начала списков, порядок
// строк после сортировки и фильтра). Про ImGui не знает.
#include "Sheaf.h"
#include <optional>
#include <string>
#include <vector>

struct ViewColumn {
    const sheaf::Column* col       = nullptr;
    uint32_t             component = 0;   // индекс в Table::components
    std::vector<size_t>  first;           // List: values[first[r] ..] — список строки r; present с теми же индексами
};

// Отбор строк «колонка = значение». У списка — хотя бы один элемент равен значению.
struct Filter {
    int         column = -1;              // индекс в ViewTable::columns, -1 — фильтра нет
    std::string text;
};

struct ViewTable {
    const sheaf::Table*     table = nullptr;
    uint32_t                base  = 0;    // сквозной номер первой строки
    std::vector<ViewColumn> columns;
    std::vector<bool>       hidden;       // по компонентам
    std::vector<uint32_t>   view;         // строки таблицы в порядке показа

    int      sort_column = -1;            // -1 — по номеру строки
    bool     sort_desc   = false;
    Filter   filter;
    uint32_t matched     = 0;
};

class Model {
public:
    // Ошибка чтения — в error, прежний файл остаётся открытым.
    bool Open(const std::string& path, std::string& error);
    bool IsOpen() const { return file_.has_value(); }

    const std::string&  Path() const  { return path_; }
    size_t              Bytes() const { return bytes_; }
    uint32_t            TotalRows() const { return total_rows_; }
    const sheaf::File&  File() const  { return *file_; }

    std::vector<ViewTable>&       Tables()       { return tables_; }
    const std::vector<ViewTable>& Tables() const { return tables_; }

    // Сквозной номер → таблица и строка в ней; nullopt — номера в файле нет.
    std::optional<std::pair<uint32_t, uint32_t>> Locate(uint32_t global) const;

    // Пересобирает ViewTable::view по sort_* и filter. Возвращает false, если текст фильтра не
    // разобрать как значение типа колонки (тогда view не меняется).
    bool Rebuild(ViewTable& t);

    std::string FormatValue(const sheaf::Column& c, uint32_t value) const;
    std::string FormatCell(const ViewColumn& vc, uint32_t row) const;   // список — «[n]»

private:
    std::optional<sheaf::File> file_;
    std::string                path_;
    size_t                     bytes_      = 0;
    uint32_t                   total_rows_ = 0;
    std::vector<ViewTable>     tables_;
    std::vector<uint32_t>      str_rank_;   // номер строки → место в алфавитном порядке; строится при первой сортировке по str
};

const char* TypeName(sheaf::Type t);
const char* EncodingName(sheaf::Encoding e);
