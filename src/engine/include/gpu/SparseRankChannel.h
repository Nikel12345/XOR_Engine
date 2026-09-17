#pragma once
#include <cstdint>
#include <vector>

class BufferManager;
struct UploadTask;

// Зеркало uint2 из sparse_rank.hlsli: раскладка слова обязана совпадать с шейдерной.
struct SparseRankWord {
	uint32_t bits;
	uint32_t base;
};

inline uint32_t SparseRankWordCount(uint32_t rows) { return (rows + 31u) / 32u; }
inline uint32_t SparseRankBytes(uint32_t rows) {
	return SparseRankWordCount(rows) * static_cast<uint32_t>(sizeof(SparseRankWord));
}

// hit_rows ОБЯЗАНЫ идти по возрастанию: base слова — бегущая сумма по ним.
void StoreSparseRank(BufferManager* bm, UploadTask* task, uint32_t rows,
                     const std::vector<uint32_t>& hit_rows);
