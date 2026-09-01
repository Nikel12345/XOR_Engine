#pragma once
#include <cstdint>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;
namespace RenderSnap { struct Regions; struct BatchLayout; }

// Строит индирект РЕГИОНАМИ: регион на группу камер, внутри — блок на камеру, и в блоке
// только команды проходов этой группы (раскладку считает RenderSnap::BuildRegions).
// num_instances у всех = 0 — компактный счётчик выживших дозаполняет scatter-каллинг атомиком.
//
// В first_instance пишется АБСОЛЮТНЫЙ адрес куска записей в out_pib: SV_InstanceID =
// first_instance + i (Vulkan), поэтому вершинники читают OutPib[instanceID] без арифметики
// блоков. Отсюда же гейт: содержимое зависит ТОЛЬКО от {ревизия батчей, число теневых камер},
// покадрово меняется одно поле num_instances — и его обнуляет culling_clear на GPU.
class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(const RenderSnap::Regions& regions, uint64_t revision,
	                               uint32_t light_cams, uint8_t slot);
	// layout нужен за прогонами групп (какие проходы делят регион) — по ним же пронумерованы
	// команды слепка; regions — за размерами и базами регионов.
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task,
	                   const RenderSnap::BatchLayout* layout, const RenderSnap::Regions& regions);
private:
	// Per-slot ключ гейта: у каждого из BUFFERING_LEVEL буферов своя пара.
	uint64_t last_revision[BUFFERING_LEVEL];
	uint32_t last_light_cams[BUFFERING_LEVEL];
	uint32_t total_size = 0;
};
