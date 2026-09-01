#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;
struct PassRegions;

// Строит индирект РЕГИОНАМИ: регион на проход, внутри — блок на каждый его дроу за кадр, в блоке
// только его команды. Раскладку штампует PassManager::StampRegions, модуль её только исполняет.
// num_instances у всех = 0 — компактный счётчик выживших дозаполняет scatter-каллинг атомиком.
//
// В first_instance пишется АБСОЛЮТНЫЙ адрес куска записей в out_pib: SV_InstanceID =
// first_instance + i (Vulkan), поэтому вершинники читают OutPib[instanceID] без арифметики
// блоков. Отсюда же гейт: содержимое зависит ТОЛЬКО от {ревизия батчей, раскладка регионов},
// покадрово меняется одно поле num_instances — и его обнуляет culling_clear на GPU.
class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(const PassRegions& regions, uint64_t revision, uint8_t slot);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task, const PassRegions& regions);
private:
	// Per-slot ключ гейта: ревизия дерева + копия раскладки, по которой залит слот.
	uint64_t last_revision[BUFFERING_LEVEL];
	std::vector<PassRegions> last_regions;
	uint32_t total_size = 0;
};
