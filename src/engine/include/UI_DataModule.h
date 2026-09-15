#pragma once
#include <cstdint>
#include <vector>

class BufferManager;
class ObjectManager;
struct UploadTask;

// Три буфера разреженного канала для текста UI: rank, index, text. Устройство канала —
// docs/render-pipeline/instance-data.md.
class UI_DataModule {
public:
	UI_DataModule();

	// Обязана идти ПЕРВОЙ из трёх инструкций: две другие читают её результат.
	void BuildStaging(ObjectManager* om);

	uint32_t CalcRankSize()  const;
	uint32_t CalcIndexSize() const;
	uint32_t CalcTextSize()  const;

	void StoreRank(BufferManager* bm, UploadTask* task);
	void StoreIndex(BufferManager* bm, UploadTask* task);
	void StoreText(BufferManager* bm, UploadTask* task);

private:
	std::vector<uint32_t> hit_rows_;
	uint32_t              rows_ = 0;
	std::vector<uint32_t> index_;
	std::vector<uint32_t> text_;

	uint32_t rank_size_  = 0;
	uint32_t index_size_ = 0;
	uint32_t text_size_  = 0;
};
