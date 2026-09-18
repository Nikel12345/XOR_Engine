#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "Colliders.h"
#include "ObjectManager.h"
#include "BaseComponents.h"

//  fn(Entity e, const std::vector<Collider>& localShapes, const Positions& P, std::size_t i)
//  Формы ЛОКАЛЬНЫЕ; P/i нужны лишь тем, кто переводит их в мир, — отрисовка рамок
//  свои параметры не называет.
namespace ColliderQuery {
	using Entity = uint32_t;

	// Словарь моделей живёт в ModelManager — он в либе Engine, которую Physics НЕ линкует (и не
	// должна). Поэтому снаружи приходит и поиск модели, и перекладка её геометрии в формы: физике
	// остаётся не знать ни про модель, ни про её сабмеши. Пустой резолвер = авто-формы отключены.
	using ModelColliders = std::function<std::vector<Collider>(ModelId)>;

	template <typename Fn>
	void ForEachActiveCollider(ObjectManager& om, SceneData* scene, const ModelColliders& colliders_of, Fn&& fn) {
		// 1) Явные формы.
		om.ForEach<Positions, ColliderComponent>(scene,
			[&](Entity e, SoAElement<Positions> p, ColliderComponent& col) {
				if (om.Has<DebugColliderTag>(scene, e)) return;
				if (col.shapes.empty()) return;   // пустой => fallback в проходе 2
				fn(e, col.shapes, p.container(), p.i());
			});

		// 2) Fallback: модель без явных форм -> формы от резолвера.
		om.ForEach<Positions, ModelComponent>(scene,
			[&](Entity e, SoAElement<Positions> p, ModelComponent& mc) {
				if (om.Has<DebugColliderTag>(scene, e)) return;
				if (om.Has<ColliderComponent>(scene, e) &&
					!om.GetComponent<ColliderComponent>(scene, e).shapes.empty())
					return;   // уже учтён явными формами
				if (!colliders_of || !mc.model) return;
				const std::vector<Collider> autoShapes = colliders_of(mc.model);
				if (autoShapes.empty()) return;

				fn(e, autoShapes, p.container(), p.i());
			});
	}
}
