#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "Colliders.h"
#include "ObjectManager.h"
#include "BaseComponents.h"

//  Единый обход «активных коллайдеров» сцены — общий для детекции контактов
//  (ContactSystem) и отладочной отрисовки (DebugColliderSystem). Два прохода:
//    1) энтити с ЯВНЫМИ формами (непустой ColliderComponent.shapes);
//    2) fallback — энтити с моделью без явных форм получают АВТО-составной
//       box-коллайдер; строит его резолвер снаружи (см. ModelColliders).
//  Визуализаторы (DebugColliderTag) и энтити без Positions пропускаются.
//
//  fn вызывается как:
//    fn(Entity e, const std::vector<Collider>& localShapes,
//       const Positions& P, std::size_t i)
//  Формы — ЛОКАЛЬНЫЕ; P/i дают трансформ энтити (нужен лишь тем, кто переводит
//  формы в мир — детекции; отрисовка рамок их игнорирует).
namespace ColliderQuery {
	using Entity = uint32_t;

	// Резолвер «ссылка на модель → её авто-формы». Словарь моделей живёт в ModelManager — он в либе
	// Engine, которую Physics НЕ линкует (и не должна), поэтому снаружи приходит и сам поиск, и
	// перекладка геометрии в формы: физике остаётся не знать ни про модель, ни про её сабмеши.
	// Пустой резолвер = авто-коллайдеров по сабмешам не будет.
	using ModelColliders = std::function<std::vector<Collider>(ModelId)>;

	template <typename Fn>
	void ForEachActiveCollider(ObjectManager& om, SceneData* scene, const ModelColliders& colliders_of, Fn&& fn) {
		// 1) Явные коллайдеры (непустой список форм).
		om.ForEach<Positions, ColliderComponent>(scene,
			[&](Entity e, SoAElement<Positions> p, ColliderComponent& col) {
				if (om.Has<DebugColliderTag>(scene, e)) return;
				if (col.shapes.empty()) return;   // пустой => fallback в проходе 2
				fn(e, col.shapes, p.container(), p.i());
			});

		// 2) Fallback: модель без явных форм -> авто-боксы по сабмешам.
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
