#include "PCH.h"
#include "BaseComponents.h"
#include "UI_DataModule.h"
#include "Utils.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "SparseRankChannel.h"
#include <bit>

UI_DataModule::UI_DataModule() {}

void UI_DataModule::BuildStaging(ObjectManager* om)
{
	SceneData* scene = om ? om->GetActiveScene() : nullptr;
	if (!scene) return;

	hit_rows_.clear();
	index_.clear();
	text_.clear();
	rows_ = 0;
	rank_size_ = index_size_ = text_size_ = 0;

	struct ArchetypeRowBase {
		const Renderable* column;
		uint32_t          base;
	};
	std::vector<ArchetypeRowBase> row_bases;
	om->ForEachArchetype<Positions, Renderable>(scene,
		[&](ComponentArray<Positions, void>* posArr, ComponentArray<Renderable, void>* renderables)
		{
			row_bases.push_back({ &renderables->data, rows_ });
			rows_ += safe_u32(posArr->size());
		});
	if (rows_ == 0) return;

	om->ForEach<Renderable, UIComponent, UITextComponent>(scene,
		[&](SoAElement<Renderable> renderable, UIComponent&, UITextComponent& txt)
		{
			const auto row_base = std::find_if(row_bases.begin(), row_bases.end(),
				[&](const ArchetypeRowBase& archetype) { return archetype.column == renderable.soa; });
			if (row_base == row_bases.end()) return;
			hit_rows_.push_back(row_base->base + safe_u32(renderable.index));

			const uint32_t offset = safe_u32(text_.size());
			const uint32_t count  = safe_u32(txt.glyphs.size());
			text_.insert(text_.end(), txt.glyphs.begin(), txt.glyphs.end());
			index_.push_back(offset);
			index_.push_back(count);
			index_.push_back(std::bit_cast<uint32_t>(txt.width_lines));
			index_.push_back(std::bit_cast<uint32_t>(txt.height_lines));
		});

	rank_size_  = SparseRankBytes(rows_);
	index_size_ = safe_u32(index_.size() * sizeof(uint32_t));
	text_size_  = safe_u32(text_.size()  * sizeof(uint32_t));
}

uint32_t UI_DataModule::CalcRankSize()  const { return rank_size_; }
uint32_t UI_DataModule::CalcIndexSize() const { return index_size_; }
uint32_t UI_DataModule::CalcTextSize()  const { return text_size_; }

void UI_DataModule::StoreRank(BufferManager* bm, UploadTask* task)
{
	StoreSparseRank(bm, task, rows_, hit_rows_);
}

void UI_DataModule::StoreIndex(BufferManager* bm, UploadTask* task)
{
	if (index_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcIndexSize(), index_.data());
}

void UI_DataModule::StoreText(BufferManager* bm, UploadTask* task)
{
	if (text_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcTextSize(), text_.data());
}
