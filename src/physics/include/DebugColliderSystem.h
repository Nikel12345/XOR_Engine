#pragma once
#include <vector>
#include <cstdint>
#include "Colliders.h"
#include "ColliderQuery.h"

class ObjectManager;
struct SceneData;

// Матрицы ЛОКАЛЬНЫЕ, мир считает движок: debug-энтити становится ребёнком owner, и
// TransformDataModule::UpdateLocalTransforms даёт world = matrix(owner) x local.
namespace DebugColliderSystem {
	using Entity = uint32_t;

	// local[16] — column-major glm, кладущая ЕДИНИЧНУЮ модель (куб [-1..1] / сфера r=1) на форму.
	struct DebugShape {
		Entity    owner;
		ShapeKind kind;
		float     local[16];
	};

	std::vector<DebugShape> CollectDebugShapes(ObjectManager& om, SceneData* scene,
		const ColliderQuery::ModelColliders& colliders_of);
}
