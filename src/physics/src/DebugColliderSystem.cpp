#include "DebugColliderSystem.h"
#include "ColliderQuery.h"
#include <glm/glm.hpp>

namespace DebugColliderSystem {

std::vector<DebugShape> CollectDebugShapes(ObjectManager& om, SceneData* scene,
	const ColliderQuery::ModelColliders& colliders_of) {
	std::vector<DebugShape> out;

	// Рамка чуть больше формы, иначе её рёбра прячутся за геометрией на depth-тесте.
	constexpr float kInflate = 1.01f;

	// Поворот и масштаб самого энтити добавит движок через иерархию — здесь только форма.
	auto emit = [&](Entity owner, ShapeKind kind, glm::vec3 half, glm::vec3 offset) {
		half *= kInflate;
		DebugShape d{};
		d.owner = owner;
		d.kind = kind;
		float* m = d.local;
		m[0] = half.x; m[1] = 0;      m[2] = 0;      m[3] = 0;
		m[4] = 0;      m[5] = half.y; m[6] = 0;      m[7] = 0;
		m[8] = 0;      m[9] = 0;      m[10] = half.z; m[11] = 0;
		m[12] = offset.x; m[13] = offset.y; m[14] = offset.z; m[15] = 1.0f;
		out.push_back(d);
	};

	// Формы берём ЛОКАЛЬНЫЕ, поэтому трансформ владельца не называем.
	ColliderQuery::ForEachActiveCollider(om, scene, colliders_of,
		[&](Entity e, const std::vector<Collider>& shapes, const Positions&, std::size_t) {
			for (const Collider& c : shapes) {
				glm::vec3 half = (c.kind == ShapeKind::Sphere) ? glm::vec3(c.radius) : c.half;
				emit(e, c.kind, half, c.offset);
			}
		});

	return out;
}

}
