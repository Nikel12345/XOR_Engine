#include "PCH.h"
#include "IndirectDataModule.h"
#include "BufferManager.h"
#include "PassManager.h"
#include "RenderSnapshot.h"
#include "BatchBuilder.h"
#include "ModelData.h"

IndirectDataModule::IndirectDataModule()
{
	for (uint64_t& r : last_revision) r = ~0ull;
	last_regions.resize(BUFFERING_LEVEL);
}

static bool SameRegions(const PassRegions& a, const PassRegions& b)
{
	if (a.per_pass.size() != b.per_pass.size()) return false;
	for (size_t i = 0; i < a.per_pass.size(); ++i) {
		const PassRegion& x = a.per_pass[i];
		const PassRegion& y = b.per_pass[i];
		if (x.command_blocks_count != y.command_blocks_count) return false;
		if (x.commands != y.commands || x.cmd_base != y.cmd_base) return false;
	}
	return true;
}

uint32_t IndirectDataModule::CalculateIndirectSize(const PassRegions& regions, uint64_t revision, uint8_t slot)
{
	if (revision == last_revision[slot] && SameRegions(regions, last_regions[slot])) return 0;

	total_size = regions.total_commands * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand));
	return total_size;
}

void IndirectDataModule::StoreIndirect(BufferManager* bm, const std::vector<PassBatches>& passes, UploadTask* task,
                                       const PassRegions& regions, uint64_t revision, uint8_t slot)
{
	if (revision == last_revision[slot] && SameRegions(regions, last_regions[slot])) return;
	last_revision[slot] = revision;
	last_regions[slot] = regions;

	// Порядок записи ОБЯЗАН совпадать со штампом регионов: пасс-мажорно, внутри прохода —
	// блок за блоком.
	for (uint32_t pass_i = 0; pass_i < passes.size(); ++pass_i) {
		if (pass_i >= regions.per_pass.size()) break;
		const PassRegion& reg = regions.per_pass[pass_i];
		const PassBatches& pass = passes[pass_i];

		for (uint32_t b = 0; b < reg.command_blocks_count; ++b) {
			for (const auto& [_, shader_batch] : pass.shader_batches) {
				for (const auto& [_, res_batch] : shader_batch.resource_batches) {
					for (const auto& [_, texture_batch] : res_batch.texture_batches) {
						for (const auto& [_, command_batch] : texture_batch.command_batches) {
							SDL_GPUIndexedIndirectDrawCommand data;
							data.num_indices = command_batch.submesh.index_count;
							data.num_instances = command_batch.instanceCount;
							data.first_index = command_batch.submesh.index_offset;
							data.vertex_offset = command_batch.submesh.vertex_offset;
							data.first_instance = command_batch.firstInstance;

							bm->UploadToTransferBuffer(task, sizeof(data), &data);
						}
					}
				}
			}
		}
	}
}
