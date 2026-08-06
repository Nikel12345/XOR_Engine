#pragma once
#include <vector>
#include <cstdint>
#include "Colliders.h"
#include "ColliderQuery.h"   // ColliderQuery::ModelLookup в сигнатуре

class ObjectManager;
struct SceneData;

// Система отладочной визуализации коллайдеров. Отдаёт ЛОКАЛЬНЫЕ (в пространстве
// модели владельца) матрицы форм — мир считает движок: debug-энтити становится
// ребёнком owner с этой локальной матрицей, а TransformDataModule::UpdateLocalTransforms
// даёт world = matrix(owner) × local. Это дебаг физической сущности (коллайдера),
// потому живёт в либе Physics, а не в рендере.
namespace DebugColliderSystem {
	using Entity = uint32_t;

	// local[16] — column-major матрица glm, кладущая единичную модель
	// (куб [-1..1] для Box, сфера r=1 для Sphere) на форму.
	struct DebugShape {
		Entity    owner;
		ShapeKind kind;
		float     local[16];
	};

	// model_of — резолвер имени модели энтити (см. ColliderQuery::ModelLookup): нужен fallback-
	// проходу, который строит авто-боксы по сабмешам.
	std::vector<DebugShape> CollectDebugShapes(ObjectManager& om, SceneData* scene,
		const ColliderQuery::ModelLookup& model_of);
}
