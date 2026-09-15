#pragma once
#include <cstdint>

class BufferManager;
class ObjectManager;
struct SceneData;
struct UploadTask;

class InstanceDataModule
{
public:
	InstanceDataModule();
	uint32_t CalculateInstanceSize(ObjectManager* objectManager, SceneData* scene);
	void StoreInstanceData(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene);
};
