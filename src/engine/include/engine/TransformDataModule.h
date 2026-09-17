#pragma once
#include <vector>

class BufferManager;
class ObjectManager;
struct SceneData;
struct BufferData;
struct UploadTask;
struct Positions;
struct LocalMatrices;

class TransformDataModule
{
public:
	void UpdateLocalTransforms(ObjectManager* objectManager, SceneData* scene);
	uint32_t CalculateTransformSize(ObjectManager* objectManager, SceneData* scene);
	void StoreTransforms(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene);
	uint32_t AskNumTransform(ObjectManager* objectManager, SceneData* scene);
private:
	uint32_t total_size = 0;
	// Поля РАЗНЫЕ: size-функция и заливка идут в разных фазах кадра, и общий счётчик означал бы,
	// что пришедший первым гасит сигнал для второго.
	uint64_t size_revision_ = ~0ull;
	uint64_t links_revision_ = ~0ull;

	// Резолв parent→(Positions*, index) стоит трёх хэш-поисков на сущность, поэтому делается один
	// раз: адреса архетипов и массивов стабильны, а индексы едут только на смене состава сущностей.
	struct LocalXformLink {
		Positions*           child_pos;
		const LocalMatrices* local;
		const Positions*     parent_pos;
		size_t child_i;
		size_t local_i;
		size_t parent_i;
	};
	std::vector<LocalXformLink> local_links_;
};
