#include "PCH.h"
#include "PIB_DataModule.h"
#include "BufferManager.h"
#include "DrawPopulation.h"
#include "EngineProfiler.h"

PIB_DataModule::PIB_DataModule()
{
    for (uint64_t& r : pib_last_revision) r = ~0ull;
}

uint32_t PIB_DataModule::CalculatePIBSizes(const DrawPopulation* population, uint64_t revision, uint8_t slot) const
{
    if (revision == pib_last_revision[slot]) return 0;
    return safe_u32(population->PibRows().size() * sizeof(uint32_t));
}

void PIB_DataModule::StorePIB(BufferManager* bm, UploadTask* task, const DrawPopulation* population,
                              uint64_t revision, uint8_t slot)
{
    if (revision == pib_last_revision[slot]) return;

    const std::vector<uint32_t>& rows = population->PibRows();
    const uint32_t bytes = safe_u32(rows.size() * sizeof(uint32_t));
    void* dst = bm->AcquireTransferWritePtr(task, bytes);
    if (!dst) return;
    pib_last_revision[slot] = revision;

    PROF_SCOPE(Sim, "     pib_copy");
    SDL_memcpy(dst, rows.data(), bytes);
}
