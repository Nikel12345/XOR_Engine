#pragma once
#include <cstdint>
#include "config.h"

class ObjectManager;
class BufferManager;
class PassManager;
class BatchBuilder;
struct UploadTask;

class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(BatchBuilder* bb, PassManager* pm, uint8_t slot);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task);
	uint32_t AskNumCommands(PassManager* pm);
private:
	// Last uploaded batch revision PER SLOT. Each of the BUFFERING_LEVEL GPU buffers
	// is independent, so a structural change must re-upload to every slot, not just
	// the one being prepared when the revision bumped.
	uint64_t last_revision[BUFFERING_LEVEL];
	uint32_t total_size = 0;
};