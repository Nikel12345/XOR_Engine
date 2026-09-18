#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "Colliders.h"
#include "ObjectManager.h"
#include "BaseComponents.h"

namespace ColliderQuery {
	using Entity = uint32_t;

	// Приходит снаружи, потому что ModelManager лежит в Engine, которую Physics НЕ линкует:
	// физике так и не нужно знать ни про модель, ни про её сабмеши. Пустой = авто-формы выключены.
	using ModelColliders = std::function<std::vector<Collider>(ModelId)>;

	template <typename Fn>
	void ForEachActiveCollider(ObjectManager& om, SceneData* scene, const ModelColliders& colliders_of, Fn&& fn) {
		om.ForEach<Positions, ColliderComponent>(scene,
			[&](Entity e, SoAElement<Positions> p, ColliderComponent& col) {
				if (om.Has<DebugColliderTag>(scene, e)) return;
				if (col.shapes.empty()) return;   // пустой => fallback в проходе 2
				fn(e, col.shapes, p.container(), p.i());
			});

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
