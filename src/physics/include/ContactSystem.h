#pragma once
#include <vector>
#include <cstdint>
#include "ColliderQuery.h"

class ObjectManager;
struct SceneData;

// Stateless: только факт пересечения в моменте — без разрешения контактов и памяти о них.
namespace ContactSystem {
	using Entity = uint32_t;

	struct Contact {
		Entity a;
		Entity b;
		float penetration;   // sphere-sphere: глубина; пара с боксом: 0 — там только факт
	};

	std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene,
		const ColliderQuery::ModelColliders& colliders_of);
}
