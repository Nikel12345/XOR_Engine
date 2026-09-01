#include "PCH.h"
#include "IndirectDataModule.h"
#include "BufferManager.h"
#include "RenderManager.h"
#include "RenderSnapshot.h"
#include "ModelData.h"

IndirectDataModule::IndirectDataModule()
{
	for (uint64_t& r : last_revision) r = ~0ull;
	for (uint32_t& c : last_light_cams) c = ~0u;
}

uint32_t IndirectDataModule::CalculateIndirectSize(const RenderSnap::Regions& regions, uint64_t revision,
                                                   uint32_t light_cams, uint8_t slot)
{
	// Ключ из двух чисел: команды зависят от ревизии батчей, разложение по камерам — от числа
	// теневых камер, которое меняется без ревизии (добавили свет, сняли ShadowCaster).
	if (revision == last_revision[slot] && light_cams == last_light_cams[slot]) return 0;
	last_revision[slot] = revision;
	last_light_cams[slot] = light_cams;

	total_size = regions.total_commands * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand));
	return total_size;
}

void IndirectDataModule::StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task,
                                       const RenderSnap::Regions& regions)
{
	const std::vector<RenderPassStep*>& ordered = pm->GetOrderedRenderPasses();

	// Порядок записи ОБЯЗАН совпадать с раскладкой AskRegions: пасс-мажорно, внутри прохода —
	// блок на камеру. Базы считает вызывающая сторона той же функцией; разъехавшись, они молча
	// отправят дроу в чужой регион.
	for (uint32_t pass_i = 0; pass_i < ordered.size(); ++pass_i) {
		if (pass_i >= regions.per_pass.size()) break;
		const RenderSnap::Region& reg = regions.per_pass[pass_i];
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
