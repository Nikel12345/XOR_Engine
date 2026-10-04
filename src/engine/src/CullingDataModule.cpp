#include "PCH.h"
#include "CullingDataModule.h"
#include "BufferManager.h"
#include "PassManager.h"
#include "RenderCommandData.h"

void CullingDataModule::Stamp(PassManager* pm, uint64_t revision, uint8_t slot)
{
    if (revision != built_revision) Rebuild(pm, revision);

    const PassRegions& regions = pm->AskRegions(slot);
    std::vector<CullPassLayout>& layout = layouts[slot];
    layout = tree_layout;
    uint32_t out = 0, cnt = 0;
    for (size_t i = 0; i < layout.size(); ++i) {
        CullPassLayout& p = layout[i];
        if (i < regions.per_pass.size()) {
            p.blocks = regions.per_pass[i].command_blocks_count;
            p.cmd_base = regions.per_pass[i].cmd_base;
        }
        p.out_base = out;  out += p.blocks * p.out_cap;
        p.cnt_base = cnt;  cnt += p.blocks * p.gl;
    }
    total_out[slot] = out;
    total_counters[slot] = cnt;
}

const CullPassLayout& CullingDataModule::Pass(uint8_t slot, uint32_t ordinal) const
{
    static const CullPassLayout none{};
    return ordinal < layouts[slot].size() ? layouts[slot][ordinal] : none;
}

uint32_t CullingDataModule::Bytes(Table table) const
{
    switch (table) {
    case Table::RecordGroup: return safe_u32(record_group.size() * sizeof(uint32_t));
    case Table::Groups:      return safe_u32(groups.size() * sizeof(CullGroupEntry));
    default:                 return safe_u32(cmd_group_level.size() * sizeof(uint32_t));
    }
}

uint32_t CullingDataModule::TableSize(Table table, uint64_t revision, uint8_t slot) const
{
    return uploaded[static_cast<size_t>(table)][slot] == revision ? 0 : Bytes(table);
}

void CullingDataModule::StoreTable(BufferManager* bm, UploadTask* task, Table table, uint64_t revision, uint8_t slot)
{
    uint64_t& last = uploaded[static_cast<size_t>(table)][slot];
    if (last == revision) return;
    last = revision;
    const uint32_t bytes = Bytes(table);
    if (bytes == 0) return;
    switch (table) {
    case Table::RecordGroup: bm->UploadToTransferBuffer(task, bytes, record_group.data());    break;
    case Table::Groups:      bm->UploadToTransferBuffer(task, bytes, groups.data());          break;
    default:                 bm->UploadToTransferBuffer(task, bytes, cmd_group_level.data()); break;
    }
}

// Обходы групп и дерева обязаны совпадать с FinalizeOffsets/StorePIB (порядок записей) и
// StoreIndirect (порядок команд).
void CullingDataModule::Rebuild(PassManager* pm, uint64_t revision)
{
    built_revision = revision;
    record_group.clear();
    groups.clear();
    cmd_group_level.clear();
    tree_layout.clear();

    std::unordered_map<const DrawGroup*, uint32_t> group_index;
    uint32_t records = 0;
    for (RenderPassStep* rp : pm->GetOrderedRenderPasses()) {
        CullPassLayout p;
        p.first_pib = records;
        p.first_cmd = safe_u32(cmd_group_level.size());
        for (const auto& [_, group] : rp->draw_groups) {
            const uint32_t index = safe_u32(groups.size());
            group_index[&group] = index;
            CullGroupEntry e;
            e.size = safe_u32(group.records.size());
            e.out_offset = p.out_cap;
            e.gl_base = p.gl;
            e.lod_count = group.lod_count;
            for (int L = 0; L < 3; ++L) e.switches[L] = group.switches[L];
            groups.push_back(e);
            record_group.insert(record_group.end(), e.size, index);
            p.out_cap += e.size * e.lod_count;
            p.gl += e.lod_count;
            p.records += e.size;
        }
        for (const auto& [_, sb] : rp->shader_batches)
            for (const auto& [_, rb] : sb.resource_batches)
                for (const auto& [_, tb] : rb.texture_batches)
                    for (const auto& [_, mb] : tb.model_batches)
                        cmd_group_level.push_back((group_index[mb.group] << 2) | mb.level);
        p.commands = safe_u32(cmd_group_level.size()) - p.first_cmd;
        records += p.records;
        tree_layout.push_back(p);
    }
}
