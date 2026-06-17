#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class ObjectManager;
struct SceneData;

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

// «Тупая» проверка факта контакта в моменте. Stateless. Sphere/Box (OBB) во всех
// сочетаниях; составные коллайдеры тестируются попарно по формам.
namespace ContactSystem {
	using Entity = uint32_t;

	struct Contact {
		Entity a;
		Entity b;
		float penetration;   // глубина для sphere-sphere; для пар с боксом = 0 (только факт)
	};

	std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene);

	// Локальные (в пространстве модели владельца) формы коллайдеров для отладочной
	// отрисовки. local[16] — column-major матрица glm, кладущая единичную модель
	// (куб [-1..1] для Box, сфера r=1 для Sphere) на форму. Мир считает движок:
	// debug-энтити становится ребёнком owner с этой локальной матрицей, а
	// TransformDataModule::UpdateLocalTransforms даёт world = matrix(owner) × local.
	struct DebugShape {
		Entity    owner;
		ShapeKind kind;
		float     local[16];
	};
	std::vector<DebugShape> CollectDebugShapes(ObjectManager& om, SceneData* scene);
}
