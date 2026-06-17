#include "ContactSystem.h"
#include "ObjectManager.h"
#include "ModelData.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace ContactSystem {

// Мировая форма: сфера или OBB. Для OBB храним центр, единичные оси и полу-размеры.
// r — радиус объемлющей сферы (broad-phase): для сферы = радиус, для бокса = |half|.
struct WorldShape {
	ShapeKind kind;
	glm::vec3 c;
	float r;
	glm::vec3 axis[3];
	glm::vec3 half;
};

struct EntityColliders {
	Entity e;
	glm::vec3 bound_c;   // объемлющая сфера всех форм (broad-phase)
	float bound_r;
	std::vector<WorldShape> shapes;
};

// Переводит формы коллайдера из локального пространства в мир матрицей Positions[i].
// Базис = столбцы матрицы (row-major): col_k; трансляция = (w,d,h).
static void BuildWorldShapes(const Positions& P, size_t i,
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
static void ComputeBound(const std::vector<WorldShape>& shapes, glm::vec3& c, float& r) {
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
static bool Overlap(const WorldShape& a, const WorldShape& b, float& pen) {
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

std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene) {
	std::vector<EntityColliders> ents;

	// 1) Явные коллайдеры (непустой список форм).
	om.ForEach<Positions, ColliderComponent>(scene,
		[&](Entity e, SoAElement<Positions> p, ColliderComponent& col) {
			if (col.shapes.empty()) return;   // пустой => fallback в проходе 2
			EntityColliders ec; ec.e = e;
			BuildWorldShapes(p.container(), p.i(), col.shapes, ec.shapes);
			ComputeBound(ec.shapes, ec.bound_c, ec.bound_r);
			ents.push_back(std::move(ec));
		});

	// 2) Fallback: энтити без явных форм получают АВТО-составной box-коллайдер —
	//    по OBB на каждый сабмеш модели из его локального AABB (min/max вершин).
	om.ForEach<Positions, ModelComponent>(scene,
		[&](Entity e, SoAElement<Positions> p, ModelComponent& mc) {
			if (om.Has<ColliderComponent>(scene, e) &&
				!om.GetComponent<ColliderComponent>(scene, e).shapes.empty())
				return;   // уже учтён явными формами
			if (!mc.model || mc.model->submeshes.empty()) return;

			std::vector<Collider> autoShapes;
			autoShapes.reserve(mc.model->submeshes.size());
			for (const auto& sm : mc.model->submeshes)
				autoShapes.push_back(Collider::Box(sm.aabb_half, sm.aabb_center));

			EntityColliders ec; ec.e = e;
			BuildWorldShapes(p.container(), p.i(), autoShapes, ec.shapes);
			ComputeBound(ec.shapes, ec.bound_c, ec.bound_r);
			ents.push_back(std::move(ec));
		});

	std::vector<Contact> contacts;
	for (size_t i = 0; i < ents.size(); ++i) {
		for (size_t j = i + 1; j < ents.size(); ++j) {
			// broad-phase: объемлющие сферы энтити
			glm::vec3 bd = ents[i].bound_c - ents[j].bound_c;
			float br = ents[i].bound_r + ents[j].bound_r;
			if (glm::dot(bd, bd) > br * br) continue;

			// narrow-phase: первая пересёкшаяся пара форм => контакт энтити
			bool hit = false; float pen = 0.0f;
			for (const auto& sa : ents[i].shapes) {
				for (const auto& sb : ents[j].shapes) {
					if (Overlap(sa, sb, pen)) { hit = true; break; }
				}
				if (hit) break;
			}
			if (hit) contacts.push_back({ ents[i].e, ents[j].e, pen });
		}
	}
	return contacts;
}

} // namespace ContactSystem
