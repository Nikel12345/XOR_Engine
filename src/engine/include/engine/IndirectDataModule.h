#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class BufferManager;
class PassManager;
struct UploadTask;
struct PassRegions;

class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(const PassRegions& regions, uint64_t revision, uint8_t slot);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task, const PassRegions& regions);
private:
	uint64_t last_revision[BUFFERING_LEVEL];
	std::vector<PassRegions> last_regions;
	uint32_t total_size = 0;
};
