#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class ObjectManager;
class BufferManager;
class PassManager;
struct SceneData;
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

    // entity -> индекс команды (model_batch) В СВОЁМ ПРОХОДЕ, по одному uint на PIB-запись,
    // в том же обходе, что PIB. scatter-каллинг по нему находит команду записи в регионе своего
    // прохода. Гейт по ревизии батчей (меняется только со структурой).
    uint32_t CalculateEntityToCmd(PassManager* pm, uint64_t revision, uint8_t slot);
    void StoreEntityToCmd(BufferManager* bm, PassManager* pm, UploadTask* task);

private:
    uint32_t ComputeElementCount(PassManager* pm) const;

    // Плоская таблица entity → строка трансформа, ОДИН последовательный проход по архетипам
    // на заливку. PIB идёт в порядке батч-дерева (произвольный относительно ECS), поэтому
    // строку надо уметь брать по entity: раньше это были два поиска в unordered_map НА КАЖДУЮ
    // запись — на 1М объектов это миллионы промахов кэша и заливка PIB на сотни мс (× слоты).
    // Таблица переводит их в один индексный доступ в плоский массив. Живёт до следующей
    // заливки (они редкие — только по смене ревизии батчей), между кадрами не валидна.
    void BuildRowTable(SceneData* scene);
    // Ревизия состава сущностей, под которую построена row_of. Таблица «сущность -> строка
    // трансформа» зависит ТОЛЬКО от набора сущностей и порядка архетипов, а StorePIB зовётся на
    // каждое изменение дерева батчей — то есть на каждую смену материала. Без этого гейта
    // 3.2 МБ memset плюс запись 800k строк выполнялись каждый тик впустую.
    uint64_t row_table_revision = ~0ull;

    static constexpr uint32_t kNoRow = 0xFFFFFFFFu;   // строки трансформа нет (= -1 в PIB)
    std::vector<uint32_t> row_of;

    uint32_t total_elements = 0;
    uint32_t e2c_elements = 0;
    // Per-slot счётчики ревизий: у каждого из BUFFERING_LEVEL буферов своя ревизия.
    uint64_t pib_last_revision[BUFFERING_LEVEL];
    uint64_t e2c_last_revision[BUFFERING_LEVEL];
};
