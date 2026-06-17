#include "PCH.h"
#include "PIB_DataModule.h"
#include "ObjectManager.h"
#include "BufferManager.h"
#include "RenderCommandData.h"
#include "RenderManager.h"
#include "BatchBuilder.h"

PIB_DataModule::PIB_DataModule()
{
    for (uint64_t& r : last_revision) r = ~0ull;
}

uint32_t PIB_DataModule::CalculatePIBSizes(BatchBuilder* bb, ObjectManager* om, PassManager* rm, uint8_t slot)
{
    uint64_t revision = bb->BatchesRevision();
    if (revision == last_revision[slot]) {
        return 0;
    }

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

    total_elements = count;
    last_revision[slot] = revision;

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
                                continue;
                            }
                            uint32_t row = arch_it->second->render_instance_base
                                         + safe_u32(scene->entity_to_index.at(entity));
                            combined.push_back(row);
                        }

    bm->UploadToTransferBuffer(task, safe_u32(combined.size()) * sizeof(uint32_t), combined.data());
}

uint32_t PIB_DataModule::CalculateEntityToBatch(BatchBuilder* bb, ObjectManager* om, PassManager* pm, uint8_t slot)
{
    return CalculatePIBSizes(bb, om, pm, slot);
}

void PIB_DataModule::StoreEntityToBatch(BufferManager* bm, PassManager* pm, UploadTask* task)
{
    uint32_t cmd_idx = 0;
    for (RenderPassStep* rp : pm->GetOrderedRenderPasses()){
        for (const auto& [_, sb] : rp->shader_batches){
            for (const auto& [_, ab] : sb.atlases_batches){
                for (const auto& [_, tb] : ab.texture_batches){
                    for (const auto& [_, mb] : tb.model_batches){
                        for (uint32_t id : mb.pib_sub_buffer) {
                            bm->UploadToPrePassTransferBuffer(task, sizeof(uint32_t), &cmd_idx);
                        }
                        cmd_idx++;
                    }
                }
            }
        }
    }
}
