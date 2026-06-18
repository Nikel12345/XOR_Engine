#include "DebugColliderSystem.h"
#include "ColliderQuery.h"
#include <glm/glm.hpp>

namespace DebugColliderSystem {

std::vector<DebugShape> CollectDebugShapes(ObjectManager& om, SceneData* scene) {
	std::vector<DebugShape> out;

	// Лёгкое раздутие рамки (на 3%), чтобы рёбра выходили чуть наружу поверхности и не
	// прятались за геометрией при depth-тесте. Центр формы не меняется.
	constexpr float kInflate = 1.03f;

	// Локальная матрица формы: column-major glm. Единичная модель ([-1..1] / r=1)
	// масштабируется полу-размером и сдвигается на offset — всё в пространстве модели
	// владельца. Поворот/масштаб самого энтити добавит движок через иерархию.
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

	// Те же активные коллайдеры, что видит детекция (явные + авто по сабмешам), но
	// рисуем их ЛОКАЛЬНЫЕ формы — трансформ владельца (P/i) не нужен, его добавит движок.
	ColliderQuery::ForEachActiveCollider(om, scene,
		[&](Entity e, const std::vector<Collider>& shapes, const Positions&, std::size_t) {
			for (const Collider& c : shapes) {
				glm::vec3 half = (c.kind == ShapeKind::Sphere) ? glm::vec3(c.radius) : c.half;
				emit(e, c.kind, half, c.offset);
			}
		});

	return out;
}

} // namespace DebugColliderSystem
