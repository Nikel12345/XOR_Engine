#include "PCH.h"
#include "IndirectDataModule.h"
#include "BufferManager.h"
#include "RenderManager.h"
#include "RenderSnapshot.h"
#include "ModelData.h"

IndirectDataModule::IndirectDataModule()
{
	for (uint64_t& r : last_revision) r = ~0ull;
	last_regions.resize(BUFFERING_LEVEL);
}

// Раскладка слота изменилась — значит изменится и содержимое команд (базы, first_instance).
static bool SameRegions(const PassRegions& a, const PassRegions& b)
{
	if (a.per_pass.size() != b.per_pass.size()) return false;
	for (size_t i = 0; i < a.per_pass.size(); ++i) {
		const PassRegion& x = a.per_pass[i];
		const PassRegion& y = b.per_pass[i];
		if (x.command_blocks_count != y.command_blocks_count) return false;
		if (x.commands != y.commands || x.pib != y.pib) return false;
		if (x.cmd_base != y.cmd_base || x.pib_base != y.pib_base) return false;
	}
	return true;
}

uint32_t IndirectDataModule::CalculateIndirectSize(const PassRegions& regions, uint64_t revision, uint8_t slot)
{
	// Ключ из двух вещей: команды зависят от ревизии батчей, их размещение — от раскладки
	// регионов, которая меняется без ревизии (добавили свет, сняли ShadowCaster).
	if (revision == last_revision[slot] && SameRegions(regions, last_regions[slot])) return 0;
	last_revision[slot] = revision;
	last_regions[slot] = regions;

	total_size = regions.total_commands * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand));
	return total_size;
}

void IndirectDataModule::StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task,
                                       const PassRegions& regions)
{
	const std::vector<RenderPassStep*>& ordered = pm->GetOrderedRenderPasses();

	// Порядок записи ОБЯЗАН совпадать со штампом регионов: пасс-мажорно, внутри прохода —
	// блок за блоком. Базы пришли оттуда же, поэтому разойтись не с чем.
	for (uint32_t pass_i = 0; pass_i < ordered.size(); ++pass_i) {
		if (pass_i >= regions.per_pass.size()) break;
		const PassRegion& reg = regions.per_pass[pass_i];
		const RenderPassStep* rp = ordered[pass_i];

		for (uint32_t b = 0; b < reg.command_blocks_count; ++b) {
			// Смещение куска от начала PIB-сегмента прохода НАКАПЛИВАЕМ по ходу обхода: вычесть
			// начало сегмента из глобального firstInstance было бы то же число, но беззнаковым
			// вычитанием, которое при рассинхроне молчит.
			uint32_t local_fi = 0;

			for (const auto& [_, shader_batch] : rp->shader_batches) {
				for (const auto& [_, atlas_batch] : shader_batch.atlases_batches) {
					for (const auto& [_, texture_batch] : atlas_batch.texture_batches) {
						for (const auto& [_, model_batch] : texture_batch.model_batches) {
							SDL_GPUIndexedIndirectDrawCommand data;
							data.num_indices = model_batch.submesh->indexCount;
							data.num_instances = 0;
							data.first_index = model_batch.submesh->indexOffset;
							data.vertex_offset = model_batch.submesh->vertexOffset;
							// Абсолютный адрес куска в out_pib (см. заголовок модуля).
							data.first_instance = reg.pib_base + b * reg.pib + local_fi;
							local_fi += model_batch.instanceCount;

							bm->UploadToTransferBuffer(task, sizeof(data), &data);
						}
					}
				}
			}
		}
	}
}
