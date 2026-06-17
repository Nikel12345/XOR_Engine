#pragma once
#include <cstdint>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;

// Dirty по ревизии батчей живёт в модуле, но модуль НЕ знает о BatchBuilder:
// ревизию передаёт вызывающая сторона числом (header-развязка). Возврат 0 при
// неизменной ревизии => store не вызовется.
class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(PassManager* pm, uint64_t revision, uint8_t slot);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task);
	uint32_t AskNumCommands(PassManager* pm);
private:
	// Per-slot: у каждого из BUFFERING_LEVEL GPU-буферов своя ревизия.
	uint64_t last_revision[BUFFERING_LEVEL];
	uint32_t total_size = 0;
};