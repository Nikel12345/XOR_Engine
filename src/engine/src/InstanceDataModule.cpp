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

// Заливка одной строки InstanceData на рисуемую энтити. ТОТ ЖЕ поархетипный обход,
// что StoreTransforms (ForEachArchetype<Positions, DrawComponent> — одна map архетипов,
// индексы 0..n-1), поэтому строка инстанса совпадает со строкой матрицы by construction.
//
// Раньше на КАЖДУЮ энтити звался UploadToTransferBuffer — 200k невстраиваемых вызовов
// с memcpy по 8 байт (это и есть те ~2.7 мс, не сама копия). Теперь, как в StoreTransforms,
// на архетип берём ОДИН write-ptr прямо в mapped transfer-буфер и пакуем весь блок за
// один проход — без вызова на энтити. Транспонирования тут нет и SIMD не нужен: источник
// DrawComponent уже AoS, выход InstanceData — тоже AoS, это strided-упаковка (alpha,flags
// лежат смежно в 12-байтовом DrawComponent → 8-байтовый InstanceData). Трафик ~4 МБ —
// упор в call-overhead, а не в полосу памяти; после его снятия копия суб-миллисекундна.
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

		// mapped-память может быть write-combined — только пишем, не читаем.
		for (size_t e = 0; e < n; ++e) {
			dst[e].alpha = D[e].alpha;
			dst[e].flags = D[e].flags;
		}
	});
}
