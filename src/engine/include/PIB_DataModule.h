#pragma once
#include <cstdint>
#include "config.h"

class ObjectManager;
class BufferManager;
class PassManager;
class BatchBuilder;
struct UploadTask;


class PIB_DataModule
{
public:
    PIB_DataModule();
    uint32_t CalculatePIBSizes(BatchBuilder* bb, ObjectManager* om, PassManager* pm, uint8_t slot);
    void StorePIB(BufferManager* bm, PassManager* pm, UploadTask* task, ObjectManager* om);
    uint32_t CalculateEntityToBatch(BatchBuilder* bb, ObjectManager* om, PassManager* pm, uint8_t slot);
    void StoreEntityToBatch(BufferManager* bm, PassManager* pm, UploadTask* task);

private:
    // Last uploaded batch revision PER SLOT. Each of the BUFFERING_LEVEL GPU buffers
    // is independent, so a structural change must re-upload to every slot, not just
    // the one being prepared when the revision bumped.
    uint64_t last_revision[BUFFERING_LEVEL];
    uint32_t total_elements = 0;
};
