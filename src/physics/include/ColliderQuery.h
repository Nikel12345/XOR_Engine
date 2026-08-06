#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include "Colliders.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "ModelData.h"

//  Единый обход «активных коллайдеров» сцены — общий для детекции контактов
//  (ContactSystem) и отладочной отрисовки (DebugColliderSystem). Два прохода:
//    1) энтити с ЯВНЫМИ формами (непустой ColliderComponent.shapes);
//    2) fallback — энтити с моделью без явных форм получают АВТО-составной
//       box-коллайдер: по OBB на каждый сабмеш из его локального AABB.
//  Визуализаторы (DebugColliderTag) и энтити без Positions пропускаются.
//
//  fn вызывается как:
//    fn(Entity e, const std::vector<Collider>& localShapes,
//       const Positions& P, std::size_t i)
//  Формы — ЛОКАЛЬНЫЕ; P/i дают трансформ энтити (нужен лишь тем, кто переводит
//  формы в мир — детекции; отрисовка рамок их игнорирует).
namespace ColliderQuery {
	using Entity = uint32_t;

	// Резолвер «имя модели → геометрия». ModelComponent хранит ИМЯ, а словарь моделей живёт в
	// ModelManager — он в либе Engine, которую Physics НЕ линкует (и не должна). Поэтому поиск
	// приходит снаружи, вызовом: физика остаётся листовой, а зависимость от каталога ассетов
	// становится явной в сигнатуре. Пустой резолвер = авто-коллайдеров по сабмешам не будет.
	using ModelLookup = std::function<const ModelData*(const std::string&)>;

	template <typename Fn>
	void ForEachActiveCollider(ObjectManager& om, SceneData* scene, const ModelLookup& model_of, Fn&& fn) {
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
				const ModelData* model = (model_of && !mc.name.empty()) ? model_of(mc.name) : nullptr;
				if (!model || model->submeshes.empty()) return;

				std::vector<Collider> autoShapes;
				autoShapes.reserve(model->submeshes.size());
				for (const auto& sm : model->submeshes)
					autoShapes.push_back(Collider::Box(sm.aabb_half, sm.aabb_center));

				fn(e, autoShapes, p.container(), p.i());
			});
	}
}
