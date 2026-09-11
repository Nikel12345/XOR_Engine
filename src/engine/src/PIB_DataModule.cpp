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
    for (uint64_t& r : e2c_last_revision) r = ~0ull;
}

uint32_t PIB_DataModule::ComputeElementCount(PassManager* rm) const
{
    uint32_t count = 0;

    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
    {
        for (const auto& [_, sb] : rp->shader_batches)
        {
            for (const auto& [_, ab] : sb.atlases_batches)
            {
                for (const auto& [_, tb] : ab.texture_batches)
                {
                    for (const auto& [_, mb] : tb.model_batches) {
                        count += mb.instanceCount;
                    }
                };
            }
        }
    }

    return count;
}

uint32_t PIB_DataModule::CalculatePIBSizes(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == pib_last_revision[slot]) return 0;

    total_elements = ComputeElementCount(rm);
    pib_last_revision[slot] = revision;

    return total_elements * sizeof(uint32_t);
}

// Отбор и арифметика строки обязаны совпадать с RecalculateInstanceOffsets и TransformDataModule.
void PIB_DataModule::BuildRowTable(SceneData* scene)
{
    row_of.assign(scene->next_entity_id, kPibNoRow);

    for (auto& [sig, arch] : scene->archetypes) {
        if (!arch.get_array<DrawComponent>() || !arch.get_array<Positions>()) continue;

        const uint32_t base = arch.render_instance_base;
        const size_t   n    = arch.entities.size();
        for (size_t i = 0; i < n; ++i) {
            const Entity e = arch.entities[i];
            if (e < row_of.size()) row_of[e] = base + safe_u32(i);
        }
    }
}

void PIB_DataModule::StorePIB(BufferManager* bm, PassManager* rm, UploadTask* task, ObjectManager* om)
{
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

    PROF_SCOPE(Sim, "     pib_gather");
    // Заливка ПИШЕТ в дерево (кэш строки в записи): дерево приватно для sim, на нём же идёт
    // заливка, а запись идемпотентна — повтор по слотам буферизации безвреден.
    uint32_t n = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (auto& [_, sb] : rp->shader_batches)
            for (auto& [_, ab] : sb.atlases_batches)
                for (auto& [_, tb] : ab.texture_batches)
                    for (auto& [_, mb] : tb.model_batches) {
                        if (n + mb.pib_sub_buffer.size() > total_elements) continue;
                        for (PibRecord& rec : mb.pib_sub_buffer) {
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

uint32_t PIB_DataModule::CalculateEntityToCmd(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == e2c_last_revision[slot]) return 0;
    e2c_last_revision[slot] = revision;
    // Счётчик свой: гейты у двух буферов раздельные, и PIB мог не пересчитаться в этом кадре.
    e2c_elements = ComputeElementCount(rm);
    return e2c_elements * sizeof(uint32_t);
}

void PIB_DataModule::StoreEntityToCmd(BufferManager* bm, PassManager* rm, UploadTask* task)
{
    // Обход и нумерация команд обязаны совпадать со StorePIB и FinalizeOffsets: индекс
    // ЛОКАЛЬНЫЙ для прохода.
    uint32_t* dst = static_cast<uint32_t*>(
        bm->AcquireTransferWritePtr(task, e2c_elements * sizeof(uint32_t)));
    if (!dst) return;

    uint32_t n = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses()) {
        uint32_t cmd_idx = 0;
        for (const auto& [_, sb] : rp->shader_batches)
            for (const auto& [_, ab] : sb.atlases_batches)
                for (const auto& [_, tb] : ab.texture_batches)
                    for (const auto& [_, mb] : tb.model_batches) {
                        const size_t cnt = mb.pib_sub_buffer.size();
                        const uint32_t word = MakeEntityToCmdWord(cmd_idx, mb.submesh.screen_size_span);
                        if (n + cnt <= e2c_elements) {
                            std::fill_n(dst + n, cnt, word);
                            n += safe_u32(cnt);
                        }
                        cmd_idx++;
                    }
    }
}
