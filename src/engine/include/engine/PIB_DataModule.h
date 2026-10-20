#pragma once
#include <cstdint>
#include "config.h"

class BufferManager;
class DrawPopulation;
struct UploadTask;


class PIB_DataModule
{
public:
    PIB_DataModule();
    uint32_t CalculatePIBSizes(const DrawPopulation* population, uint64_t revision, uint8_t slot) const;
    void StorePIB(BufferManager* bm, UploadTask* task, const DrawPopulation* population,
                  uint64_t revision, uint8_t slot);

private:
    uint64_t pib_last_revision[BUFFERING_LEVEL];
};
