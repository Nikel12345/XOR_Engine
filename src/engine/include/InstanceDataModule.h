#pragma once
#include <cstdint>

class BufferManager;
class ObjectManager;
struct SceneData;
struct UploadTask;

// Per-instance данные (alpha, flags) → DEFAULT_INSTANCE_BUFFER. Заливаются в ТОМ ЖЕ порядке
// архетипов, что и трансформы (TransformDataModule), поэтому row из PositionIndexBuffer
// индексирует и матрицу, и этот буфер. Обновляется каждый кадр (пока без dirty). Буфер
// Dynamic — из него возможны удаления.
class InstanceDataModule
{
public:
	InstanceDataModule();
	uint32_t CalculateInstanceSize(ObjectManager* objectManager, SceneData* scene);
	void StoreInstanceData(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene);
};
