#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class ObjectManager;
struct SceneData;

// Явный коллайдер на энтити. Если есть — перекрывает модельную сферу.
// radius и offset — В МИРОВЫХ единицах: центр = мировая трансляция энтити + offset.
// (Никакого масштабирования матрицей — полный предсказуемый контроль.)
struct ColliderComponent {
	float radius = 0.5f;
	glm::vec3 offset = glm::vec3(0.0f);
};

// «Тупая» проверка факта контакта в моменте: сфера-vs-сфера по bound-сферам
// моделей (ModelComponent) и мировым трансформам (Positions). Stateless.
namespace ContactSystem {
	using Entity = uint32_t;

	struct Contact {
		Entity a;
		Entity b;
		float penetration;   // глубина пересечения: (rA + rB) - dist, > 0
	};

	// Возвращает все пересекающиеся пары энтити активной сцены (broad-phase O(n^2)).
	std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene);
}
