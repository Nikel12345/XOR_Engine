#include "PCH.h"
#include "InstanceDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "BaseComponents.h"

struct InstanceData {
	float    alpha;
	uint32_t flags;
};

InstanceDataModule::InstanceDataModule()
{
}

// Размер = число рисуемых строк × sizeof(InstanceData). Отбор и порядок архетипов — ТЕ ЖЕ,
// что в TransformDataModule (инвариант «строка инстанс-данных = строка матрицы»).
uint32_t InstanceDataModule::CalculateInstanceSize(ObjectManager* om, SceneData* scene)
{
	uint32_t total = 0;
	om->ForEachArchetype<Positions, DrawComponent>(
		scene,
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<DrawComponent, void>*)
	{
		total += safe_u32(posArr->size()) * sizeof(InstanceData);
	});
	return total;
}

void InstanceDataModule::StoreInstanceData(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene)
{
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>*,
			ComponentArray<DrawComponent, void>* drawArr)
	{
		const std::vector<DrawComponent>& D = drawArr->data;
		const size_t n = D.size();
		if (n == 0) return;

		InstanceData* dst = static_cast<InstanceData*>(
			bm->AcquireTransferWritePtr(task, safe_u32(n * sizeof(InstanceData))));
		if (!dst) return;

		for (size_t e = 0; e < n; ++e) {
			dst[e].alpha = D[e].alpha;
			dst[e].flags = D[e].flags;
		}
	});
}
