#pragma once
#include <cstdint>
#include <vector>

class BufferManager;
class ObjectManager;
struct UploadTask;

class UI_ClipDataModule {
public:
	UI_ClipDataModule();

	void BuildStaging(ObjectManager* om);

	uint32_t CalcRankSize()  const;
	uint32_t CalcIndexSize() const;
	uint32_t CalcRectSize()  const;

	void StoreRank(BufferManager* bm, UploadTask* task);
	void StoreIndex(BufferManager* bm, UploadTask* task);
	void StoreRect(BufferManager* bm, UploadTask* task);

private:
	std::vector<uint32_t> hit_rows;
	uint32_t              rows = 0;
	std::vector<uint32_t> indices;
	std::vector<float>    rects;

	uint32_t rank_size  = 0;
	uint32_t index_size = 0;
	uint32_t rect_size  = 0;
};
