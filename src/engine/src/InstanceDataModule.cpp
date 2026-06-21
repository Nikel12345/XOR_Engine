#include "PCH.h"
#include "InstanceDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "BaseComponents.h"

// GPU-раскладка per-instance данных — нужна только здесь. Дублирует struct InstanceData в
// main_pass.vert.hlsl; заливается в DEFAULT_INSTANCE_BUFFER в порядке архетипов (как матрицы).
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

// Заливка по строке на каждую рисуемую энтити, в том же обходе, что StoreTransforms
// (ForEach<DrawComponent, Positions>) — row совпадает с матрицей.
void InstanceDataModule::StoreInstanceData(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene)
{
	om->ForEach<DrawComponent, Positions>(scene, [&](const DrawComponent& draw, SoAElement<Positions>) {
		InstanceData inst{ draw.alpha, draw.flags };
		bm->UploadToTransferBuffer(task, sizeof(inst), &inst);
	});
}
