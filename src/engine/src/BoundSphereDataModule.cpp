#include "PCH.h"
#include "BoundSphereDataModule.h"
#include "RenderManager.h"
#include "BufferManager.h"
#include "ModelManager.h"

BoundSphereDataModule::BoundSphereDataModule() {}

uint32_t BoundSphereDataModule::CalculateSphereSize(PassManager* pm, ModelManager* mm)
{
	if (!mm->CheckDirtySpheres()) {
		return 0;
	}
	total_size = 0;
    auto render_passes = pm->GetOrderedRenderPasses();

	for (const RenderPassStep* rp : render_passes) {
		for (const auto& [_, shader_batch] : rp->shader_batches) {
			for (const auto& [_, atlas_batch] : shader_batch.atlases_batches) {
				for (const auto& [_, texture_batch] : atlas_batch.texture_batches) {
					for (const auto& [_, model_batch] : texture_batch.model_batches) {
						total_size += 1;
					}
				}
			}
		}
	}
	mm->CommitSpheres();
	return total_size * sizeof(glm::vec4);
}

void BoundSphereDataModule::StoreSpheres(BufferManager* bm, UploadTask* task, PassManager* pm)
{
	auto render_passes = pm->GetOrderedRenderPasses();
	for (const RenderPassStep* rp : render_passes) {
		for (const auto& [_, shader_batch] : rp->shader_batches) {
			for (const auto& [_, atlas_batch] : shader_batch.atlases_batches) {
				for (const auto& [_, texture_batch] : atlas_batch.texture_batches) {
					for (const auto& [_, model_batch] : texture_batch.model_batches) {
						bm->UploadToTransferBuffer(task, sizeof(glm::vec4), &model_batch.submesh->sphere);
					}
				}
			}
		}
	}
}
