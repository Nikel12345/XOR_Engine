#include "PCH.h"
#include "BaseComponents.h"
#include "UI_ClipDataModule.h"
#include "Utils.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "SparseRankChannel.h"
#include "TransformRows.h"

UI_ClipDataModule::UI_ClipDataModule() {}

void UI_ClipDataModule::BuildStaging(ObjectManager* om)
{
	SceneData* scene = om ? om->GetActiveScene() : nullptr;
	if (!scene) return;

	hit_rows.clear();
	indices.clear();
	rects.clear();
	rows = 0;
	rank_size = index_size = rect_size = 0;

	const TransformRows transform_rows = CollectTransformRows(om, scene);
	rows = transform_rows.count;
	if (rows == 0) return;

	om->ForEach<Renderable, UIClipComponent>(scene,
		[&](SoAElement<Renderable> renderable, UIClipComponent& clip)
		{
			const std::optional<uint32_t> row = TransformRowOf(transform_rows, renderable);
			if (!row) return;
			hit_rows.push_back(*row);
			indices.push_back(clip.rect_index);
		});

	om->ForEach<UIClipRectComponent>(scene,
		[&](UIClipRectComponent& rect)
		{
			const size_t offset = size_t(rect.index) * 4;
			if (rects.size() < offset + 4) rects.resize(offset + 4, 0.0f);
			rects[offset + 0] = rect.left;
			rects[offset + 1] = rect.top;
			rects[offset + 2] = rect.right;
			rects[offset + 3] = rect.bottom;
		});

	rank_size  = SparseRankBytes(rows);
	index_size = safe_u32(indices.size() * sizeof(uint32_t));
	rect_size  = safe_u32(rects.size()   * sizeof(float));
}

uint32_t UI_ClipDataModule::CalcRankSize()  const { return rank_size; }
uint32_t UI_ClipDataModule::CalcIndexSize() const { return index_size; }
uint32_t UI_ClipDataModule::CalcRectSize()  const { return rect_size; }

void UI_ClipDataModule::StoreRank(BufferManager* bm, UploadTask* task)
{
	StoreSparseRank(bm, task, rows, hit_rows);
}

void UI_ClipDataModule::StoreIndex(BufferManager* bm, UploadTask* task)
{
	if (indices.empty()) return;
	bm->UploadToTransferBuffer(task, CalcIndexSize(), indices.data());
}

void UI_ClipDataModule::StoreRect(BufferManager* bm, UploadTask* task)
{
	if (rects.empty()) return;
	bm->UploadToTransferBuffer(task, CalcRectSize(), rects.data());
}
