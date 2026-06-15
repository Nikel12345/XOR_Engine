#pragma once
#include <cstdint>

class ObjectManager;
class BufferManager;
class PassManager;
class BatchBuilder;
struct UploadTask;

class IndirectDataModule
{
public:
	IndirectDataModule();
	uint32_t CalculateIndirectSize(BatchBuilder* bb, PassManager* pm);
	void StoreIndirect(BufferManager* bm, PassManager* pm, UploadTask* task);
	uint32_t AskNumCommands(PassManager* pm);
private:
	// Last batch revision this module uploaded for; re-uploads only when it differs.
	uint64_t last_batches_revision = ~0ull;
	uint32_t total_size = 0;
};