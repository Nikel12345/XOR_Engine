#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

//  Компоненты коллайдеров — ЧИСТЫЕ ДАННЫЕ, без логики.
//  Этот заголовок включают и системы физики (ContactSystem,
//  DebugColliderSystem), и игра, когда вешает коллайдеры на энтити.
//  Поэтому здесь нет ни ObjectManager, ни математики — только структуры.

// Форма коллайдера. Задаётся в ЛОКАЛЬНОМ пространстве модели и переводится в мир
// матрицей энтити (Positions): позиция, поворот и масштаб. Бокс становится OBB.
enum class ShapeKind : uint8_t { Sphere, Box };

struct Collider {
	ShapeKind kind   = ShapeKind::Sphere;
	glm::vec3 offset = glm::vec3(0.0f);   // локальный центр формы
	float     radius = 0.5f;              // Sphere: радиус (×макс. масштаб энтити)
	glm::vec3 half   = glm::vec3(0.5f);   // Box: локальные полу-размеры (×масштаб по осям)

	static Collider Sphere(float r, glm::vec3 off = glm::vec3(0.0f)) {
		return { ShapeKind::Sphere, off, r, glm::vec3(0.0f) };
	}
	static Collider Box(glm::vec3 half_extents, glm::vec3 off = glm::vec3(0.0f)) {
		return { ShapeKind::Box, off, 0.0f, half_extents };
	}
};

// Составной коллайдер: несколько форм на энтити. Пустой список => fallback на
// объемлющую сферу модели (ModelComponent).
struct ColliderComponent {
	std::vector<Collider> shapes;
};

// Тег визуализации: энтити рисует рамку коллайдера и НЕ участвует в детекции
// контактов (иначе его debug-модель попала бы в fallback как авто-коллайдер).
struct DebugColliderTag {};
