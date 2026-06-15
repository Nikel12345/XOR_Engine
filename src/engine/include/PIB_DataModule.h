#pragma once
#include <cstdint>

class ObjectManager;
class BufferManager;
class PassManager;
class BatchBuilder;
struct UploadTask;


class PIB_DataModule
{
public:
    PIB_DataModule();
    uint32_t CalculatePIBSizes(BatchBuilder* bb, ObjectManager* om, PassManager* pm);
    void StorePIB(BufferManager* bm, PassManager* pm, UploadTask* task, ObjectManager* om);
    uint32_t CalculateEntityToBatch(BatchBuilder* bb, ObjectManager* om, PassManager* pm);
    void StoreEntityToBatch(BufferManager* bm, PassManager* pm, UploadTask* task);

private:
    uint64_t last_batches_revision = ~0ull;
    uint32_t total_elements = 0;
};
