#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class BufferManager;
class ObjectManager;
class MaterialManager;
struct SceneData;
struct UploadTask;

class TextureStateDataModule {
public:
	TextureStateDataModule();

	// Обязана идти ПЕРВОЙ из трёх инструкций: две другие читают её результат.
	uint32_t CalculateRankSize(ObjectManager* om, SceneData* scene, uint64_t revision, uint8_t slot);
	void     StoreRank(BufferManager* bm, UploadTask* task);

	uint32_t CalculateIndexSize(uint64_t revision, uint8_t slot);
	void     StoreIndex(BufferManager* bm, UploadTask* task);

	uint32_t CalculateStateSize(ObjectManager* om, SceneData* scene);
	void     StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene,
	                    MaterialManager* mtm);

private:
	std::vector<uint32_t> hit_rows_;
	std::vector<uint32_t> hit_ofs_;
	uint32_t rows_ = 0;

	uint64_t last_rank_revision[BUFFERING_LEVEL];
	uint64_t last_index_revision[BUFFERING_LEVEL];
};
