#include "CollisionShapes.h"
#include "BaseComponents.h"   // полное определение Positions
#include <algorithm>
#include <cmath>

namespace Collision {

// Переводит формы коллайдера из локального пространства в мир матрицей Positions[i].
// Базис = столбцы матрицы (row-major): col_k; трансляция = (w,d,h).
void BuildWorldShapes(const Positions& P, std::size_t i,
	const std::vector<Collider>& shapes, std::vector<WorldShape>& out)
{
	const glm::vec3 c0{ P.x[i], P.a[i], P.e[i] };
	const glm::vec3 c1{ P.y[i], P.b[i], P.f[i] };
	const glm::vec3 c2{ P.z[i], P.c[i], P.g[i] };
	const glm::vec3 t { P.w[i], P.d[i], P.h[i] };
	const float l0 = glm::length(c0), l1 = glm::length(c1), l2 = glm::length(c2);
	const glm::vec3 a0 = l0 > 1e-6f ? c0 / l0 : glm::vec3(1, 0, 0);
	const glm::vec3 a1 = l1 > 1e-6f ? c1 / l1 : glm::vec3(0, 1, 0);
	const glm::vec3 a2 = l2 > 1e-6f ? c2 / l2 : glm::vec3(0, 0, 1);
	const float smax = std::max({ l0, l1, l2 });

	for (const Collider& col : shapes) {
		WorldShape ws{};
		ws.kind = col.kind;
		ws.c = t + c0 * col.offset.x + c1 * col.offset.y + c2 * col.offset.z;
		if (col.kind == ShapeKind::Sphere) {
			ws.r = col.radius * smax;
		} else {
			ws.axis[0] = a0; ws.axis[1] = a1; ws.axis[2] = a2;
			ws.half = glm::vec3(col.half.x * l0, col.half.y * l1, col.half.z * l2);
			ws.r = glm::length(ws.half);   // объемлющая сфера OBB
		}
		out.push_back(ws);
	}
}

// Объемлющая сфера набора форм (broad-phase энтити).
void ComputeBound(const std::vector<WorldShape>& shapes, glm::vec3& c, float& r) {
	c = glm::vec3(0.0f); r = 0.0f;
	if (shapes.empty()) return;
	for (const auto& s : shapes) c += s.c;
	c /= float(shapes.size());
	for (const auto& s : shapes) r = std::max(r, glm::length(c - s.c) + s.r);
}

static bool SphereBox(const WorldShape& s, const WorldShape& b) {
	glm::vec3 d = s.c - b.c;
	glm::vec3 closest = b.c;
	for (int k = 0; k < 3; ++k) {
		float dist = glm::dot(d, b.axis[k]);
		dist = std::clamp(dist, -b.half[k], b.half[k]);
		closest += dist * b.axis[k];
	}
	glm::vec3 v = s.c - closest;
	return glm::dot(v, v) <= s.r * s.r;
}

// SAT для OBB-OBB: разделены, если есть ось без перекрытия проекций.
static bool BoxBox(const WorldShape& A, const WorldShape& B) {
	const glm::vec3 T = B.c - A.c;
	auto overlaps = [&](glm::vec3 L) -> bool {
		if (glm::dot(L, L) < 1e-8f) return true;   // вырожденная ось (параллельные рёбра) — пропуск
		float ra = A.half.x * std::abs(glm::dot(A.axis[0], L))
		         + A.half.y * std::abs(glm::dot(A.axis[1], L))
		         + A.half.z * std::abs(glm::dot(A.axis[2], L));
		float rb = B.half.x * std::abs(glm::dot(B.axis[0], L))
		         + B.half.y * std::abs(glm::dot(B.axis[1], L))
		         + B.half.z * std::abs(glm::dot(B.axis[2], L));
		return std::abs(glm::dot(T, L)) <= ra + rb;
	};
	for (int k = 0; k < 3; ++k) {
		if (!overlaps(A.axis[k])) return false;
		if (!overlaps(B.axis[k])) return false;
	}
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			if (!overlaps(glm::cross(A.axis[i], B.axis[j]))) return false;
	return true;
}

// Диспетчер: факт пересечения двух форм. pen — глубина только для sphere-sphere.
bool Overlap(const WorldShape& a, const WorldShape& b, float& pen) {
	pen = 0.0f;
	if (a.kind == ShapeKind::Sphere && b.kind == ShapeKind::Sphere) {
		glm::vec3 d = a.c - b.c;
		float rs = a.r + b.r;
		float d2 = glm::dot(d, d);
		if (d2 < rs * rs) { pen = rs - std::sqrt(d2); return true; }
		return false;
	}
	if (a.kind == ShapeKind::Sphere) return SphereBox(a, b);
	if (b.kind == ShapeKind::Sphere) return SphereBox(b, a);
	return BoxBox(a, b);
}

} // namespace Collision
