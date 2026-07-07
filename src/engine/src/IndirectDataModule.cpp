#include "PCH.h"
#include "IndirectDataModule.h"
#include "BufferManager.h"
#include "RenderManager.h"
#include "ModelData.h"

IndirectDataModule::IndirectDataModule()
{
}

uint32_t IndirectDataModule::CalculateIndirectSize(PassManager* pm, uint32_t num_cameras)
{
	total_size = num_cameras * AskNumCommands(pm) * sizeof(SDL_GPUIndexedIndirectDrawCommand);
	return total_size;
}

void IndirectDataModule::StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task, uint32_t num_cameras)
{
	const std::vector<RenderPassStep*>& render_passes = pm->GetOrderedRenderPasses();

	// num_cameras копий команд подряд: блок камеры c = [c*num_commands, (c+1)*num_commands).
	// num_instances = 0 у всех — scatter атомарно наберёт компактный счётчик выживших.
	// first_instance/first_index/vertex_offset/num_indices — исходные (компактация НА МЕСТЕ,
	// внутри диапазона команды, поэтому first_instance не меняется).
	for (uint32_t c = 0; c < num_cameras; ++c) {
		for (const RenderPassStep* rp : render_passes) {
			for (const auto& [_, shader_batch] : rp->shader_batches) {
				for (const auto& [_, atlas_batch] : shader_batch.atlases_batches) {
					for (const auto& [_, texture_batch] : atlas_batch.texture_batches) {
						for (const auto& [_, model_batch] : texture_batch.model_batches) {
							SDL_GPUIndexedIndirectDrawCommand data;
							data.num_indices = model_batch.submesh->indexCount;
							data.num_instances = 0;
							data.first_index = model_batch.submesh->indexOffset;
							data.vertex_offset = model_batch.submesh->vertexOffset;
							data.first_instance = model_batch.firstInstance;

							bm->UploadToTransferBuffer(task, sizeof(data), &data);
						}
					}
				}
			}
		}
	}
}

uint32_t IndirectDataModule::AskNumCommands(PassManager* pm)
{
	uint32_t num_model_batches = 0;
	const std::vector<RenderPassStep*>& render_passes = pm->GetOrderedRenderPasses();

	for (const RenderPassStep* rp : render_passes) {
		for (const auto& [_, shader_batch] : rp->shader_batches) {
			for (const auto& [_, atlas_batch] : shader_batch.atlases_batches) {
				for (const auto& [_, texture_batch] : atlas_batch.texture_batches) {
					num_model_batches += safe_u32(texture_batch.model_batches.size());
				}
			}
		}
	}

	return num_model_batches;
}
