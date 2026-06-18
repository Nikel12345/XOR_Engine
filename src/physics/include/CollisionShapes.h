#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "Colliders.h"

struct Positions;   // SoA-трансформы; полное определение нужно только в .cpp

// ============================================================
//  Геометрическое ядро узкой фазы — БЕЗ ECS и без ObjectManager.
//  Переводит локальные Collider'ы в мировые формы и проверяет их пересечение.
//  Знает раскладку Positions (row-major столбцы, трансляция в w/d/h) — это
//  «физическое» знание о GPU-трансформах, общее для всех систем физики.
// ============================================================
namespace Collision {

	// Мировая форма: сфера или OBB. Для OBB храним центр, единичные оси и полу-размеры.
	// r — радиус объемлющей сферы (broad-phase): для сферы = радиус, для бокса = |half|.
	struct WorldShape {
		ShapeKind kind;
		glm::vec3 c;
		float     r;
		glm::vec3 axis[3];
		glm::vec3 half;
	};

	// Переводит формы коллайдера из локального пространства в мир матрицей Positions[i].
	void BuildWorldShapes(const Positions& P, std::size_t i,
		const std::vector<Collider>& shapes, std::vector<WorldShape>& out);

	// Объемлющая сфера набора форм (broad-phase энтити).
	void ComputeBound(const std::vector<WorldShape>& shapes, glm::vec3& c, float& r);

	// Факт пересечения двух форм. pen — глубина только для sphere-sphere (иначе 0).
	bool Overlap(const WorldShape& a, const WorldShape& b, float& pen);

} // namespace Collision
