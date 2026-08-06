#pragma once
#include <vector>
#include <cstdint>
#include "ColliderQuery.h"   // ColliderQuery::ModelLookup в сигнатуре

class ObjectManager;
struct SceneData;

// Система детекции контактов. «Тупая» проверка факта пересечения в моменте.
// Stateless. Sphere/Box (OBB) во всех сочетаниях; составные коллайдеры
// тестируются попарно по формам. Геометрия — в ядре Collision (CollisionShapes.h),
// компоненты — в Colliders.h.
namespace ContactSystem {
	using Entity = uint32_t;

	struct Contact {
		Entity a;
		Entity b;
		float penetration;   // глубина для sphere-sphere; для пар с боксом = 0 (только факт)
	};

	// model_of — резолвер имени модели энтити (см. ColliderQuery::ModelLookup): нужен fallback-
	// проходу, который строит авто-боксы по сабмешам.
	std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene,
		const ColliderQuery::ModelLookup& model_of);
}
