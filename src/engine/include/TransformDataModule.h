#pragma once
#include <vector>

class BufferManager;
class ObjectManager;
struct SceneData;
struct BufferData;
struct UploadTask;
struct ReadBackTask;

class TransformDataModule
{
public:
	TransformDataModule();
	void UpdateLocalTransforms(ObjectManager* objectManager, SceneData* scene);
	uint32_t CalculateTransformSize(ObjectManager* objectManager, SceneData* scene);
	void StoreTransforms(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene);
	uint32_t AskNumTransform(ObjectManager* objectManager, SceneData* scene);

	uint32_t ReadBackCullingCountSize() { return sizeof(uint32_t); };
	void ReadBackCullingCountReader(BufferManager* bm, ReadBackTask* task);

	uint32_t CalculateOutTransformSize();
private:
	uint32_t total_size = 0;
	std::span<const std::byte> size_ptr;
	std::vector<float> staging_buffer_;
};
