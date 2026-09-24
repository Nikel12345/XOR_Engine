#include "PCH.h"
#include "BaseComponents.h"
#include "PIB_DataModule.h"
#include "ObjectManager.h"
#include "BufferManager.h"
#include "RenderCommandData.h"
#include "ModelData.h"
#include "PassManager.h"
#include "EngineProfiler.h"

PIB_DataModule::PIB_DataModule()
{
    for (uint64_t& r : pib_last_revision) r = ~0ull;
}

uint32_t PIB_DataModule::ComputeElementCount(PassManager* rm) const
{
    uint32_t count = 0;

    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (const auto& [_, group] : rp->draw_groups)
            count += safe_u32(group.records.size());

    return count;
}

uint32_t PIB_DataModule::CalculatePIBSizes(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == pib_last_revision[slot]) return 0;

    total_elements = ComputeElementCount(rm);
    return total_elements * sizeof(uint32_t);
}

// Отбор и арифметика строки обязаны совпадать с RecalculateInstanceOffsets и TransformDataModule.
void PIB_DataModule::BuildRowTable(SceneData* scene)
{
    row_of.assign(scene->next_entity_id, kPibNoRow);

    for (auto& [sig, arch] : scene->archetypes) {
        if (!arch.get_array<Renderable>() || !arch.get_array<Positions>()) continue;

        const uint32_t base = arch.render_instance_base;
        const size_t   n    = arch.entities.size();
        for (size_t i = 0; i < n; ++i) {
            const Entity e = arch.entities[i];
            if (e < row_of.size()) row_of[e] = base + safe_u32(i);
        }
    }
}

void PIB_DataModule::StorePIB(BufferManager* bm, PassManager* rm, UploadTask* task, ObjectManager* om,
                              uint64_t revision, uint8_t slot)
{
    if (revision == pib_last_revision[slot]) return;

    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    const uint64_t rev = om->EntityRevision();
    const bool refresh = (rev != row_table_revision);
    if (refresh) {
        PROF_SCOPE(Sim, "     pib_row_table");
        BuildRowTable(scene);
        row_table_revision = rev;
    }

    uint32_t* dst = static_cast<uint32_t*>(
        bm->AcquireTransferWritePtr(task, total_elements * sizeof(uint32_t)));
    if (!dst) return;
    pib_last_revision[slot] = revision;

    PROF_SCOPE(Sim, "     pib_gather");
    // Заливка ПИШЕТ в дерево (кэш строки в записи): дерево приватно для sim, на нём же идёт
    // заливка, а запись идемпотентна — повтор по слотам буферизации безвреден.
    uint32_t n = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (auto& [_, group] : rp->draw_groups) {
            if (n + group.records.size() > total_elements) continue;
            for (PibRecord& rec : group.records) {
                if (refresh || rec.row == kPibNoRow) {
                    rec.row = (rec.entity < row_of.size()) ? row_of[rec.entity] : kPibNoRow;
                }
#ifndef NDEBUG
                else {
                    // Расхождение кэша с таблицей = состав сущностей изменился без
                    // ++entity_revision. Наяву это не краш, а чужая матрица у одного
                    // объекта из миллиона.
                    assert(rec.row == ((rec.entity < row_of.size()) ? row_of[rec.entity] : kPibNoRow));
                }
#endif
                dst[n++] = rec.row;
            }
        }
}
