#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "Colliders.h"

struct Positions;   // SoA-трансформы; полное определение нужно только в .cpp

namespace Collision {

	// r — радиус объемлющей сферы (broad-phase): для сферы = её радиус, для бокса = |half|.
	struct WorldShape {
		ShapeKind kind;
		glm::vec3 c;
		float     r;
		glm::vec3 axis[3];
		glm::vec3 half;
	};

	void BuildWorldShapes(const Positions& P, std::size_t i,
		const std::vector<Collider>& shapes, std::vector<WorldShape>& out);

	void ComputeBound(const std::vector<WorldShape>& shapes, glm::vec3& c, float& r);

	// pen — глубина только для sphere-sphere, иначе 0: там проверяется лишь факт.
	bool Overlap(const WorldShape& a, const WorldShape& b, float& pen);

} // namespace Collision
