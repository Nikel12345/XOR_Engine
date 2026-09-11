#include "PCH.h"
#include "TextureStateDataModule.h"
#include "BaseComponents.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "MaterialManager.h"
#include "MaterialData.h"
#include "SparseRankChannel.h"


static inline uint32_t ElementCells(const MaterialComponent& mc)
{
	return safe_u32(mc.materials.size()) * MAX_VARIATIVE_SLOTS;
}

TextureStateDataModule::TextureStateDataModule()
{
	for (uint64_t& r : last_rank_revision)  r = ~0ull;
	for (uint64_t& r : last_index_revision) r = ~0ull;
}

uint32_t TextureStateDataModule::CalculateRankSize(ObjectManager* om, SceneData* scene, uint64_t revision, uint8_t slot)
{
	if (revision == last_rank_revision[slot]) return 0;
	last_rank_revision[slot] = revision;

	struct ArchBase { const Positions* col; uint32_t base; };
	std::vector<ArchBase> bases;
	rows_ = 0;
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<DrawComponent, void>*)
	{
		bases.push_back(ArchBase{ &posArr->data, rows_ });
		rows_ += safe_u32(posArr->size());
	});

	// Тот же обход и порядок, что у StoreState: смещение, посчитанное здесь той же прогрессией,
	// указывает ровно на ячейки, которые там лягут.
	hit_rows_.clear();
	hit_ofs_.clear();
	uint32_t running = 0;
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions> pos, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
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

void TextureStateDataModule::StoreRank(BufferManager* bm, UploadTask* task)
{
	StoreSparseRank(bm, task, rows_, hit_rows_);
}

uint32_t TextureStateDataModule::CalculateIndexSize(uint64_t revision, uint8_t slot)
{
	if (revision == last_index_revision[slot]) return 0;
	last_index_revision[slot] = revision;
	return safe_u32(hit_ofs_.size() * sizeof(uint32_t));
}

void TextureStateDataModule::StoreIndex(BufferManager* bm, UploadTask* task)
{
	if (hit_ofs_.empty()) return;
	bm->UploadToTransferBuffer(task, safe_u32(hit_ofs_.size() * sizeof(uint32_t)), hit_ofs_.data());
}

uint32_t TextureStateDataModule::CalculateStateSize(ObjectManager* om, SceneData* scene)
{
	uint32_t cells = 0;
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
	{
		cells += ElementCells(mc);
	});

	return cells * sizeof(uint32_t);
}

void TextureStateDataModule::StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om,
	SceneData* scene, MaterialManager* mtm)
{
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
	{
		for (const MaterialRef& m : mc.materials) {
			uint32_t cells[MAX_VARIATIVE_SLOTS] = {};

			// Резолв ТИХИЙ: GetMaterial логирует промах, а вызов идёт на каждую сущность.
			const Material* mat = nullptr;
			if (mtm && !m.name.empty()) {
				const auto& materials = mtm->GetMaterials();
				auto it = materials.find(m.name);
				if (it != materials.end()) mat = it->second.get();
			}

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
