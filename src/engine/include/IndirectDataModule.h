#pragma once
#include <cstdint>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;
namespace RenderSnap { struct Regions; }

// Строит индирект РЕГИОНАМИ: регион на проход, внутри — command_blocks_count блоков (по числу
// дроу прохода за кадр, обычно = его камерам), в блоке только его команды (раскладка — AskRegions).
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
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task,
	                   const RenderSnap::Regions& regions);
private:
	// Per-slot ключ гейта: у каждого из BUFFERING_LEVEL буферов своя пара.
	uint64_t last_revision[BUFFERING_LEVEL];
	uint32_t last_light_cams[BUFFERING_LEVEL];
	uint32_t total_size = 0;
};
