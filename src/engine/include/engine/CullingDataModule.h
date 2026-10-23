#pragma once
#include <bit>
#include <cstdint>
#include <vector>
#include "config.h"
#include "ResourceId.h"
#include "ModelData.h"

class BufferManager;
class PassManager;
class ModelManager;
struct UploadTask;
struct PassBatches;

inline constexpr uint32_t kGroupLevelBits = std::bit_width(MAX_MODEL_LODS - 1u);

// Должно совпадать с GroupLayout в culling_scatter/culling_fixup.
struct CullGroupLayout {
    uint32_t size = 0;
    uint32_t out_offset = 0;
    uint32_t first_group_level = 0;
    uint32_t lod_count = 1;
    uint32_t model = 0;
};
static_assert(sizeof(CullGroupLayout) == 5 * sizeof(uint32_t));

// Должно совпадать с CullModel в culling_scatter. Только скаляры: с float4 запись в std430 выровнялась
// бы до 16 байт, а в DXIL — нет.
struct CullModel {
    float sphere[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
    float switches[MAX_MODEL_LODS - 1] = {};
};
static_assert(sizeof(CullModel) == (4 + MAX_MODEL_LODS - 1) * sizeof(float));

// Раскладка прохода для отсева: из дерева (записи, групповые уровни, команды) и из регионов слота
// (блоки, база команд). out_pib и счётчики — по блокам прохода, проходы подряд.
struct CullPassLayout {
    uint32_t first_pib = 0, records = 0;
    uint32_t cmd_base = 0, commands = 0, first_cmd = 0;
    uint32_t blocks = 0, group_level_count = 0, out_capacity = 0;
    uint32_t out_base = 0, cnt_base = 0;
};

class CullingDataModule {
public:
    CullingDataModule();

    // Sim-поток, фаза размеров: дерево и регионы слота уже устоялись. Идемпотентно в пределах кадра.
    void Stamp(PassManager* pm, ModelManager* mm, const std::vector<PassBatches>& passes, uint64_t revision, uint8_t slot);

    const CullPassLayout& Pass(uint8_t slot, uint32_t ordinal) const;
    uint32_t TotalOut(uint8_t slot) const { return total_out[slot]; }
    uint32_t TotalCounters(uint8_t slot) const { return total_counters[slot]; }

    enum class Table : uint8_t { RecordToGroup, GroupLayouts, CommandToGroupLevel, Models, Count };
    uint32_t TableSize(Table table, uint64_t revision, uint8_t slot) const;
    void     StoreTable(BufferManager* bm, UploadTask* task, Table table, uint64_t revision, uint8_t slot);

private:
    void Rebuild(const std::vector<PassBatches>& passes, uint64_t revision);
    void FillModels(ModelManager* mm);
    uint32_t ModelIndex(ModelId model);
    uint32_t Bytes(Table table) const;

    std::vector<uint32_t>        record_to_group;
    std::vector<CullGroupLayout> group_layouts;
    std::vector<uint32_t>        command_to_group_level;
    std::vector<ModelId>         model_ids;
    std::vector<CullModel>       models;

    uint64_t built_revision = ~0ull;
    uint64_t filled_spheres_revision = ~0ull;
    std::vector<CullPassLayout> tree_layout;
    std::vector<CullPassLayout> layouts[BUFFERING_LEVEL];
    uint32_t total_out[BUFFERING_LEVEL] = {};
    uint32_t total_counters[BUFFERING_LEVEL] = {};
    uint64_t uploaded[static_cast<size_t>(Table::Count)][BUFFERING_LEVEL];
};
