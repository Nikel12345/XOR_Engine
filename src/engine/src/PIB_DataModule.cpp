#include "PCH.h"
#include "PIB_DataModule.h"
#include "ObjectManager.h"
#include "BufferManager.h"
#include "RenderCommandData.h"
#include "RenderManager.h"

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

void PIB_DataModule::StorePIB(BufferManager* bm, PassManager* rm, UploadTask* task, ObjectManager* om)
{
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    std::vector<uint32_t> combined;
    combined.reserve(total_elements);
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (const auto& [_, sb] : rp->shader_batches)
            for (const auto& [_, ab] : sb.atlases_batches)
                for (const auto& [_, tb] : ab.texture_batches)
                    for (const auto& [_, mb] : tb.model_batches)
                        for (uint32_t entity : mb.pib_sub_buffer) {
                            auto arch_it = scene->entity_to_archetype.find(entity);
                            if (arch_it == scene->entity_to_archetype.end()) {
                                SDL_Log("StorePIB: entity %u not in scene (stale pib slot)", entity);
                                // -1, а НЕ пропуск: пропуск сдвинул бы все последующие записи
                                // относительно firstInstance батчей. -1 понимают и culling-шейдер
                                // (прокидывает дальше), и вершинник (вырожденная позиция).
                                combined.push_back(0xFFFFFFFFu);
                                continue;
                            }
                            // Transformless (нет Positions): энтити батчится, но строки в
                            // transform/instance/sphere-буферах не имеет (их домен {Positions ∧
                            // Draw}, см. RecalculateInstanceOffsets). -1 → каллинг скаттерит
                            // запись безусловно (сферы нет), а строку никто не читает: такой
                            // sp строит позицию сам (напр. скайбокс — из камеры). С протухшими
                            // -1 не конфликтует: удаление энтити пересобирает батчи и PIB.
                            if (!arch_it->second->get_array<Positions>()) {
                                combined.push_back(0xFFFFFFFFu);
                                continue;
                            }
                            uint32_t row = arch_it->second->render_instance_base
                                         + safe_u32(scene->entity_to_index.at(entity));
                            combined.push_back(row);
                        }

    bm->UploadToTransferBuffer(task, safe_u32(combined.size()) * sizeof(uint32_t), combined.data());
}

uint32_t PIB_DataModule::CalculateEntityToCmd(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == e2c_last_revision[slot]) return 0;
    e2c_last_revision[slot] = revision;
    // Один uint на PIB-запись — тот же размер, что PIB.
    return ComputeElementCount(rm) * sizeof(uint32_t);
}

void PIB_DataModule::StoreEntityToCmd(BufferManager* bm, PassManager* rm, UploadTask* task)
{
    // Тот же обход, что StorePIB/StoreIndirect/FinalizeOffsets: команды нумеруются подряд
    // по проходам (cmd_idx++ на model_batch), а каждая запись получает индекс своей команды.
    std::vector<uint32_t> combined;
    combined.reserve(total_elements);
    uint32_t cmd_idx = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (const auto& [_, sb] : rp->shader_batches)
            for (const auto& [_, ab] : sb.atlases_batches)
                for (const auto& [_, tb] : ab.texture_batches)
                    for (const auto& [_, mb] : tb.model_batches) {
                        for (size_t n = 0; n < mb.pib_sub_buffer.size(); ++n)
                            combined.push_back(cmd_idx);
                        cmd_idx++;
                    }

    bm->UploadToTransferBuffer(task, safe_u32(combined.size()) * sizeof(uint32_t), combined.data());
}
