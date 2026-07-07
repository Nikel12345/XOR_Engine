#pragma once
#include <cstdint>
#include "config.h"

class ObjectManager;
class BufferManager;
class PassManager;
struct UploadTask;


// Dirty по ревизии батчей живёт в модуле, но модуль НЕ знает о BatchBuilder:
// ревизию передаёт вызывающая сторона числом (header-развязка — правка BatchBuilder.h
// не пересобирает этот TU). Возврат 0 при неизменной ревизии => store не вызовется.
class PIB_DataModule
{
public:
    PIB_DataModule();
    uint32_t CalculatePIBSizes(PassManager* pm, uint64_t revision, uint8_t slot);
    void StorePIB(BufferManager* bm, PassManager* pm, UploadTask* task, ObjectManager* om);

    // entity -> глобальный индекс команды (model_batch), по одному uint на PIB-запись,
    // в том же обходе, что PIB. scatter-каллинг по нему находит команду записи. Гейт по
    // ревизии батчей (меняется только со структурой). Индекс сам кодирует проход.
    uint32_t CalculateEntityToCmd(PassManager* pm, uint64_t revision, uint8_t slot);
    void StoreEntityToCmd(BufferManager* bm, PassManager* pm, UploadTask* task);

private:
    uint32_t ComputeElementCount(PassManager* pm) const;

    uint32_t total_elements = 0;
    // Per-slot счётчики ревизий: у каждого из BUFFERING_LEVEL буферов своя ревизия.
    uint64_t pib_last_revision[BUFFERING_LEVEL];
    uint64_t e2c_last_revision[BUFFERING_LEVEL];
};
