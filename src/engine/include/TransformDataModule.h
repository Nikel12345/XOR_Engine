#pragma once
#include <vector>

class BufferManager;
class ObjectManager;
struct SceneData;
struct BufferData;
struct UploadTask;
struct Positions;        // для кэша связей иерархии — нужны только указатели
struct LocalMatrices;

class TransformDataModule
{
public:
	TransformDataModule();
	void UpdateLocalTransforms(ObjectManager* objectManager, SceneData* scene);
	uint32_t CalculateTransformSize(ObjectManager* objectManager, SceneData* scene);
	void StoreTransforms(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene);
	uint32_t AskNumTransform(ObjectManager* objectManager, SceneData* scene);
private:
	uint32_t total_size = 0;

	// Кэш связей иерархии (см. UpdateLocalTransforms). Резолв parent→(Positions*,index)
	// стоит 3 хэш-поиска в unordered_map на сущность каждый кадр; делаем его ОДИН раз и
	// переиспользуем, перестраивая только при структурном изменении ECS (dirty-протокол
	// ObjectManager). Адреса Archetype (std::map) и ComponentArray (unique_ptr) стабильны;
	// индексы/содержимое векторов меняются лишь на add/delete/load — они и взводят dirty.
	struct LocalXformLink {
		Positions*           child_pos;    // куда писать world (Positions ребёнка)
		const LocalMatrices* local;        // локальная матрица ребёнка
		const Positions*     parent_pos;   // world родителя
		size_t child_i;
		size_t local_i;
		size_t parent_i;
	};
	std::vector<LocalXformLink> local_links_;
};
