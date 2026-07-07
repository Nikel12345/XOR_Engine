#pragma once
#include <cstdint>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;

// Строит ПО-КАМЕРНЫЙ индирект: num_cameras копий команд подряд, у каждой копии
// num_instances = 0 (компактный счётчик выживших дозаполняет scatter-каллинг атомиком).
// Per-frame (num_instances обязан обнуляться каждый кадр), поэтому БЕЗ revision-гейта:
// размер небольшой (num_cameras * commands), заливка дешёвая.
class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(PassManager* pm, uint32_t num_cameras);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task, uint32_t num_cameras);
	uint32_t AskNumCommands(PassManager* pm);
private:
	uint32_t total_size = 0;
};
