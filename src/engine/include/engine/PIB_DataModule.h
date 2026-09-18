#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class ObjectManager;
class BufferManager;
class PassManager;
struct SceneData;
struct UploadTask;


class PIB_DataModule
{
public:
    PIB_DataModule();
    uint32_t CalculatePIBSizes(PassManager* pm, uint64_t revision, uint8_t slot);
    void StorePIB(BufferManager* bm, PassManager* pm, UploadTask* task, ObjectManager* om,
                  uint64_t revision, uint8_t slot);

    uint32_t CalculateEntityToCmd(PassManager* pm, uint64_t revision, uint8_t slot);
    void StoreEntityToCmd(BufferManager* bm, PassManager* pm, UploadTask* task,
                          uint64_t revision, uint8_t slot);

private:
    uint32_t ComputeElementCount(PassManager* pm) const;

    void BuildRowTable(SceneData* scene);
    uint64_t row_table_revision = ~0ull;
    std::vector<uint32_t> row_of;

    uint32_t total_elements = 0;
    uint32_t e2c_elements = 0;
    uint64_t pib_last_revision[BUFFERING_LEVEL];
    uint64_t e2c_last_revision[BUFFERING_LEVEL];
};
