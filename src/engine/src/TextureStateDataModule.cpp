#include "PCH.h"
#include "TextureStateDataModule.h"
#include "BaseComponents.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "MaterialManager.h"
#include "MaterialData.h"
#include "SparseRankChannel.h"


// Полагается на StateSection(i, L) == i: секции лежат в порядке обхода materials.
static inline uint32_t ElementCells(SoAElement<Renderable> r)
{
	return safe_u32(r.container().materials[r.i()].size()) * MAX_VARIATIVE_SLOTS;
}

TextureStateDataModule::TextureStateDataModule()
{
	for (uint64_t& r : last_rank_revision)  r = ~0ull;
	for (uint64_t& r : last_index_revision) r = ~0ull;
}

uint32_t TextureStateDataModule::CalculateRankSize(ObjectManager* om, SceneData* scene, uint64_t revision, uint8_t slot)
{
	if (revision == last_rank_revision[slot]) return 0;

	struct ArchBase { const Positions* col; uint32_t base; };
	std::vector<ArchBase> bases;
	rows_ = 0;
	om->ForEachArchetype<Positions, Renderable>(scene,
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<Renderable, void>*)
	{
		bases.push_back(ArchBase{ &posArr->data, rows_ });
		rows_ += safe_u32(posArr->size());
	});

	// Тот же обход и порядок, что у StoreState: смещение, посчитанное здесь той же прогрессией,
	// указывает ровно на ячейки, которые там лягут.
	hit_rows_.clear();
	hit_ofs_.clear();
	uint32_t running = 0;
	om->ForEach<Positions, Renderable, TextureStateComponent>(scene,
		[&](SoAElement<Positions> pos, SoAElement<Renderable> mc, TextureStateComponent&)
	{
		const Positions* col = pos.soa;
		for (const ArchBase& a : bases) {
			if (a.col != col) continue;
			hit_rows_.push_back(a.base + safe_u32(pos.i()));
			hit_ofs_.push_back(running);
			break;
		}
		running += ElementCells(mc);
	});

	return SparseRankBytes(rows_);
}

void TextureStateDataModule::StoreRank(BufferManager* bm, UploadTask* task, uint64_t revision, uint8_t slot)
{
	if (revision == last_rank_revision[slot]) return;
	last_rank_revision[slot] = revision;
	StoreSparseRank(bm, task, rows_, hit_rows_);
}

uint32_t TextureStateDataModule::CalculateIndexSize(uint64_t revision, uint8_t slot)
{
	if (revision == last_index_revision[slot]) return 0;
	return safe_u32(hit_ofs_.size() * sizeof(uint32_t));
}

void TextureStateDataModule::StoreIndex(BufferManager* bm, UploadTask* task, uint64_t revision, uint8_t slot)
{
	if (revision == last_index_revision[slot]) return;
	last_index_revision[slot] = revision;   // носителей может не быть: писать нечего, но ревизия обработана
	if (hit_ofs_.empty()) return;
	bm->UploadToTransferBuffer(task, safe_u32(hit_ofs_.size() * sizeof(uint32_t)), hit_ofs_.data());
}

uint32_t TextureStateDataModule::CalculateStateSize(ObjectManager* om, SceneData* scene)
{
	uint32_t cells = 0;
	om->ForEach<Positions, Renderable, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, SoAElement<Renderable> mc, TextureStateComponent&)
	{
		cells += ElementCells(mc);
	});

	return cells * sizeof(uint32_t);
}

void TextureStateDataModule::StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om,
	SceneData* scene, MaterialManager* mtm)
{
	om->ForEach<Positions, Renderable, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, SoAElement<Renderable> mc, TextureStateComponent&)
	{
		for (const MaterialSlot& m : mc.container().materials[mc.i()]) {
			uint32_t cells[MAX_VARIATIVE_SLOTS] = {};

			// Резолв ТИХИЙ: GetMaterial логирует промах, а вызов идёт на каждую сущность.
			const Material* mat = mtm ? mtm->GetMaterial(m.per_lod[0]) : nullptr;

			if (mat) {
				const VariativeRoles vr = CollectVariativeRoles(*mat);
				for (uint32_t c = 0; c < vr.count; ++c)
					for (const auto& [role, v] : m.states)
						if (role == vr.role[c]) { cells[c] = v; break; }
			}

			bm->UploadToTransferBuffer(task, sizeof(cells), cells);
		}
	});
}
