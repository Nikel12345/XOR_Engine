#pragma once
#include <vector>
#include <cstdint>

// Одномерный аллокатор диапазонов в элементах — docs/gpu/geometry/pools.md.
class RangeAllocator
{
public:
    struct Range {
        uint32_t first = 0;
        uint32_t count = 0;   // 0 = пусто/не выделено
    };

    Range Allocate(uint32_t count);
    void  Free(Range r);

    uint32_t Top() const { return top_; }
    void Reset();

    size_t   FreeBlocks() const { return free_.size(); }
    uint32_t FreeTotal()  const;

private:
    // ИНВАРИАНТ: отсортирован по first, соседние блоки слиты, пустых нет. На нём держится и
    // слияние (смотрим только двух соседей), и опускание вершины.
    std::vector<Range> free_;
    uint32_t top_ = 0;
};
