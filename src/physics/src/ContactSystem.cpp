#include "ContactSystem.h"
#include "ColliderQuery.h"
#include "CollisionShapes.h"
#include <glm/glm.hpp>

namespace ContactSystem {

using Collision::WorldShape;

struct EntityColliders {
	Entity e;
	glm::vec3 bound_c;   // объемлющая сфера всех форм (broad-phase)
	float bound_r;
	std::vector<WorldShape> shapes;
};

std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene,
	const ColliderQuery::ModelLookup& model_of) {
	// Собираем мировые формы всех активных коллайдеров сцены (явные + авто по сабмешам).
	std::vector<EntityColliders> ents;
	ColliderQuery::ForEachActiveCollider(om, scene, model_of,
		[&](Entity e, const std::vector<Collider>& shapes, const Positions& P, std::size_t i) {
			EntityColliders ec; ec.e = e;
			Collision::BuildWorldShapes(P, i, shapes, ec.shapes);
			Collision::ComputeBound(ec.shapes, ec.bound_c, ec.bound_r);
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
					if (Collision::Overlap(sa, sb, pen)) { hit = true; break; }
				}
				if (hit) break;
			}
			if (hit) contacts.push_back({ ents[i].e, ents[j].e, pen });
		}
	}
	return contacts;
}

} // namespace ContactSystem
