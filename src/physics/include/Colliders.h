#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

// Заголовок включает и игра (вешает коллайдеры на энтити) — поэтому тут только структуры,
// без ObjectManager и без математики.

// Формы задаются в ЛОКАЛЬНОМ пространстве модели, в мир их переводит матрица энтити
// (Positions) — с поворотом и масштабом, поэтому бокс становится OBB.
enum class ShapeKind : uint8_t { Sphere, Box };

struct Collider {
	ShapeKind kind   = ShapeKind::Sphere;
	glm::vec3 offset = glm::vec3(0.0f);
	float     radius = 0.5f;              // Sphere: множится на МАКСИМАЛЬНЫЙ масштаб энтити
	glm::vec3 half   = glm::vec3(0.5f);   // Box: множится на масштаб ПО ОСЯМ

	static Collider Sphere(float r, glm::vec3 off = glm::vec3(0.0f)) {
		return { ShapeKind::Sphere, off, r, glm::vec3(0.0f) };
	}
	static Collider Box(glm::vec3 half_extents, glm::vec3 off = glm::vec3(0.0f)) {
		return { ShapeKind::Box, off, 0.0f, half_extents };
	}
};

// Пустой список => fallback: авто-боксы по сабмешам модели (ColliderQuery, проход 2).
struct ColliderComponent {
	std::vector<Collider> shapes;
};

// Рисует рамку и НЕ участвует в детекции: иначе его debug-модель попала бы в fallback
// как авто-коллайдер.
struct DebugColliderTag {};
