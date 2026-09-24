#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;

// Должно совпадать с GroupEntry в culling_scatter/culling_fixup.
struct CullGroupEntry {
    uint32_t size = 0;
    uint32_t out_offset = 0;
    uint32_t gl_base = 0;
    uint32_t lod_count = 1;
    float    switches[3] = {};
    uint32_t pad = 0;
};
static_assert(sizeof(CullGroupEntry) == 32);

// Раскладка прохода для отсева: из дерева (записи, групповые уровни, команды) и из регионов слота
// (блоки, база команд). out_pib и счётчики — по блокам прохода, проходы подряд.
struct CullPassLayout {
    uint32_t first_pib = 0, records = 0;
    uint32_t cmd_base = 0, commands = 0, first_cmd = 0;
    uint32_t blocks = 0, gl = 0, out_cap = 0;
    uint32_t out_base = 0, cnt_base = 0;
};

class CullingDataModule {
public:
    // Sim-поток, фаза размеров: дерево и регионы слота уже устоялись. Идемпотентно в пределах кадра.
    void Stamp(PassManager* pm, uint64_t revision, uint8_t slot);

    const CullPassLayout& Pass(uint8_t slot, uint32_t ordinal) const;
    uint32_t TotalOut(uint8_t slot) const { return total_out[slot]; }
    uint32_t TotalCounters(uint8_t slot) const { return total_counters[slot]; }

    enum class Table : uint8_t { RecordGroup, Groups, CmdGroupLevel };
    uint32_t TableSize(Table table, uint64_t revision, uint8_t slot) const;
    void     StoreTable(BufferManager* bm, UploadTask* task, Table table, uint64_t revision, uint8_t slot);

private:
    void Rebuild(PassManager* pm, uint64_t revision);
    uint32_t Bytes(Table table) const;

    std::vector<uint32_t>       record_group;      // запись PIB -> группа
    std::vector<CullGroupEntry> groups;
    std::vector<uint32_t>       cmd_group_level;   // команда -> (группа << 2) | уровень

    uint64_t built_revision = ~0ull;
    std::vector<CullPassLayout> tree_layout;
    std::vector<CullPassLayout> layouts[BUFFERING_LEVEL];
    uint32_t total_out[BUFFERING_LEVEL] = {};
    uint32_t total_counters[BUFFERING_LEVEL] = {};
    uint64_t uploaded[3][BUFFERING_LEVEL] = {
        { ~0ull, ~0ull, ~0ull }, { ~0ull, ~0ull, ~0ull }, { ~0ull, ~0ull, ~0ull } };
};
