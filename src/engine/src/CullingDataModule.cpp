#include "PCH.h"
#include "CullingDataModule.h"
#include "BufferManager.h"
#include "PassManager.h"
#include "BatchBuilder.h"
#include "ModelManager.h"
#include "EngineProfiler.h"

CullingDataModule::CullingDataModule()
{
    for (auto& per_slot : uploaded)
        for (uint64_t& r : per_slot) r = ~0ull;
}

void CullingDataModule::Stamp(PassManager* pm, ModelManager* mm, const std::vector<PassBatches>& passes,
                              uint64_t revision, uint8_t slot)
{
    if (revision != built_revision) {
        PROF_SCOPE(Sim, "     culling_rebuild");
        Rebuild(passes, revision);
        filled_spheres_revision = ~0ull;
    }
    if (mm->SpheresRevision() != filled_spheres_revision) FillModels(mm);

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
        p.out_base = out;  out += p.blocks * p.out_capacity;
        p.cnt_base = cnt;  cnt += p.blocks * p.group_level_count;
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
    case Table::RecordToGroup:       return safe_u32(record_to_group.size() * sizeof(uint32_t));
    case Table::GroupLayouts:        return safe_u32(group_layouts.size() * sizeof(CullGroupLayout));
    case Table::CommandToGroupLevel: return safe_u32(command_to_group_level.size() * sizeof(uint32_t));
    default:                         return safe_u32(models.size() * sizeof(CullModel));
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
    case Table::RecordToGroup:       bm->UploadToTransferBuffer(task, bytes, record_to_group.data());        break;
    case Table::GroupLayouts:        bm->UploadToTransferBuffer(task, bytes, group_layouts.data());          break;
    case Table::CommandToGroupLevel: bm->UploadToTransferBuffer(task, bytes, command_to_group_level.data()); break;
    default:                         bm->UploadToTransferBuffer(task, bytes, models.data());                 break;
    }
}

uint32_t CullingDataModule::ModelIndex(ModelId model)
{
    for (size_t i = 0; i < model_ids.size(); ++i)
        if (model_ids[i] == model) return safe_u32(i);
    model_ids.push_back(model);
    return safe_u32(model_ids.size() - 1);
}

// Обходы групп и дерева обязаны совпадать с DrawPopulation::Scatter и BatchBuilder::ApplyPopulation
// (порядок записей) и с BatchBuilder::BuildLayoutSnapshot и StoreIndirect (порядок команд).
void CullingDataModule::Rebuild(const std::vector<PassBatches>& passes, uint64_t revision)
{
    built_revision = revision;
    record_to_group.clear();
    group_layouts.clear();
    command_to_group_level.clear();
    model_ids.clear();
    tree_layout.clear();

    uint32_t records = 0;
    for (const PassBatches& pass : passes) {
        CullPassLayout p;
        p.first_pib = records;
        p.first_cmd = safe_u32(command_to_group_level.size());
        const uint32_t pass_group_base = safe_u32(group_layouts.size());
        for (const InstanceGroup& group : pass.instance_groups) {
            const uint32_t index = safe_u32(group_layouts.size());
            CullGroupLayout e;
            e.size = group.size;
            e.out_offset = p.out_capacity;
            e.first_group_level = p.group_level_count;
            e.lod_count = group.lod_count;
            e.model = ModelIndex(group.root_model);
            group_layouts.push_back(e);
            record_to_group.insert(record_to_group.end(), e.size, index);
            p.out_capacity += e.size * e.lod_count;
            p.group_level_count += e.lod_count;
            p.records += e.size;
        }
        for (const auto& [_, sb] : pass.shader_batches)
            for (const auto& [_, rb] : sb.resource_batches)
                for (const auto& [_, tb] : rb.texture_batches)
                    for (const auto& [_, mb] : tb.command_batches)
                        command_to_group_level.push_back(((pass_group_base + mb.instance_group_index) << kGroupLevelBits) | mb.level);
        p.commands = safe_u32(command_to_group_level.size()) - p.first_cmd;
        records += p.records;
        tree_layout.push_back(p);
    }
}

static glm::vec4 UnionSpheres(const glm::vec4& a, const glm::vec4& b)
{
    glm::vec3 d = glm::vec3(b) - glm::vec3(a);
    float dist = glm::length(d);
    if (a.w >= dist + b.w) return a;
    if (b.w >= dist + a.w) return b;
    float R = (dist + a.w + b.w) * 0.5f;
    glm::vec3 c = glm::vec3(a) + d * ((R - a.w) / dist);
    return glm::vec4(c, R);
}

// w = -1 читается шейдером отсева как «геометрии нет»: записи такой модели скаттерятся безусловно.
static glm::vec4 ModelSphere(const ModelData* model)
{
    if (!model || model->submeshes.empty()) return glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
    glm::vec4 sphere = model->submeshes[0].sphere;
    for (size_t i = 1; i < model->submeshes.size(); ++i)
        sphere = UnionSpheres(sphere, model->submeshes[i].sphere);
    return sphere;
}

void CullingDataModule::FillModels(ModelManager* mm)
{
    filled_spheres_revision = mm->SpheresRevision();
    models.assign(model_ids.size(), CullModel{});
    for (size_t i = 0; i < model_ids.size(); ++i) {
        const ModelData* root = mm->FindModel(model_ids[i]);
        if (!root) continue;

        // Сфера одна на все уровни: по ней же выбирается уровень, и тест видимости верен при любом.
        glm::vec4 sphere(0.0f, 0.0f, 0.0f, -1.0f);
        const uint32_t levels = mm->LevelCount(model_ids[i]);
        for (uint32_t L = 0; L < levels; ++L) {
            const glm::vec4 s = ModelSphere(mm->FindModel(mm->LevelModel(model_ids[i], L)));
            if (s.w < 0.0f) continue;
            sphere = sphere.w < 0.0f ? s : UnionSpheres(sphere, s);
        }

        CullModel& entry = models[i];
        for (int c = 0; c < 4; ++c) entry.sphere[c] = sphere[c];
        for (size_t L = 0; L < root->lods.size() && L + 1 < MAX_MODEL_LODS; ++L)
            entry.switches[L] = root->lods[L].switch_px;
    }
}
