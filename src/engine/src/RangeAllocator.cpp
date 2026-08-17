#include "PCH.h"
#include "RangeAllocator.h"
#include <algorithm>

RangeAllocator::Range RangeAllocator::Allocate(uint32_t count)
{
    if (count == 0) return {};

    for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].count < count) continue;
        Range out{ free_[i].first, count };
        if (free_[i].count == count) free_.erase(free_.begin() + i);
        else { free_[i].first += count; free_[i].count -= count; }   // остаток остаётся дырой
        return out;
    }

    Range out{ top_, count };
    top_ += count;
    return out;
}

void RangeAllocator::Free(Range r)
{
    if (r.count == 0) return;

    auto it = std::lower_bound(free_.begin(), free_.end(), r.first,
        [](const Range& a, uint32_t f) { return a.first < f; });
    it = free_.insert(it, r);

    // Сначала правый сосед, потом левый: после слияния с правым блок вырастает, и левый должен
    // видеть уже итоговую границу — иначе цепочка из трёх смежных дыр слипнется не полностью.
    if (it + 1 != free_.end() && it->first + it->count == (it + 1)->first) {
        it->count += (it + 1)->count;
        free_.erase(it + 1);
    }
    if (it != free_.begin() && (it - 1)->first + (it - 1)->count == it->first) {
        (it - 1)->count += it->count;
        free_.erase(it);
    }

    // Дыра, дошедшая до вершины, дырой не хранится — вершина просто опускается. Ради этого
    // перезагрузка сцены не оставляет буфер навсегда раздутым: когда освободится весь хвост,
    // занятое схлопнется обратно, а не будет вечно жить выше старого содержимого.
    if (!free_.empty() && free_.back().first + free_.back().count == top_) {
        top_ = free_.back().first;
        free_.pop_back();
    }
}

void RangeAllocator::Reset()
{
    free_.clear();
    top_ = 0;
}

uint32_t RangeAllocator::FreeTotal() const
{
    uint32_t total = 0;
    for (const Range& r : free_) total += r.count;
    return total;
}
