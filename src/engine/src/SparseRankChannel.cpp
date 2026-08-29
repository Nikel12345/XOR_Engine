#include "PCH.h"
#include "SparseRankChannel.h"
#include "Utils.h"
#include "BufferManager.h"

// Сколько слов копится в локальном буфере до сброса в трансфер. Вызов заливки несёт проверки
// таска и границ буфера-назначения, и платить их поэлементно дорого (ЗАМЕРЕНО на домене 800k,
// Release: поэлементно 14.0 мс против 1.3 мс пачками). Буфер ОГРАНИЧЕННЫЙ и локальный — это не
// CPU-отражение буфера.
static constexpr size_t FLUSH_WORDS = 4096;

void StoreSparseRank(BufferManager* bm, UploadTask* task, uint32_t rows,
                     const std::vector<uint32_t>& hit_rows)
{
	const uint32_t words = SparseRankWordCount(rows);
	if (words == 0) return;

	std::vector<SparseRankWord> chunk(FLUSH_WORDS);
	size_t h = 0;
	uint32_t base = 0;

	for (uint32_t w0 = 0; w0 < words; w0 += FLUSH_WORDS) {
		const uint32_t n = (words - w0 < FLUSH_WORDS) ? words - w0 : safe_u32(FLUSH_WORDS);
		for (uint32_t i = 0; i < n; ++i) {
			// base — состояние ДО слова, поэтому пишется раньше, чем поглощаются его носители.
			chunk[i].base = base;
			uint32_t bits = 0;
			const uint32_t row_end = (w0 + i + 1u) * 32u;
			while (h < hit_rows.size() && hit_rows[h] < row_end) {
				bits |= 1u << (hit_rows[h] & 31u);
				++h; ++base;
			}
			chunk[i].bits = bits;
		}
		bm->UploadToTransferBuffer(task, safe_u32(size_t(n) * sizeof(SparseRankWord)), chunk.data());
	}
}
