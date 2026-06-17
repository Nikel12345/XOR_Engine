#include "ContactSystem.h"
#include "ObjectManager.h"
#include "ModelData.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace ContactSystem {

struct WorldSphere { Entity e; glm::vec3 c; float r; };

// Объединяющая (консервативная) сфера всех сабмешей модели в model-space.
static void ModelSphere(const ModelData* m, glm::vec3& center, float& radius) {
	center = glm::vec3(0.0f);
	radius = 0.0f;
	if (!m || m->submeshes.empty()) return;

	glm::vec3 sum(0.0f);
	for (const auto& s : m->submeshes) sum += glm::vec3(s.sphere);
	center = sum / float(m->submeshes.size());

	for (const auto& s : m->submeshes) {
		float d = glm::length(center - glm::vec3(s.sphere)) + s.sphere.w;
		radius = std::max(radius, d);
	}
}

std::vector<Contact> DetectContacts(ObjectManager& om, SceneData* scene) {
	std::vector<WorldSphere> spheres;

	// 1) Явные коллайдеры: мировой центр = трансляция энтити + offset, радиус = col.radius.
	om.ForEach<Positions, ColliderComponent>(scene,
		[&](Entity e, SoAElement<Positions> p, ColliderComponent& col) {
			const Positions& P = p.container();
			const size_t i = p.i();
			glm::vec3 t{ P.w[i], P.d[i], P.h[i] };
			spheres.push_back({ e, t + col.offset, col.radius });
		});

	// 2) Fallback: энтити без явного коллайдера берут сферу из модели.
	//    Центр переводится матрицей (world = M * model_center), радиус масштабируется
	//    макс. длиной столбца-базиса.
	om.ForEach<Positions, ModelComponent>(scene,
		[&](Entity e, SoAElement<Positions> p, ModelComponent& mc) {
			if (om.Has<ColliderComponent>(scene, e)) return;   // явный коллайдер уже учтён

			const Positions& P = p.container();
			const size_t i = p.i();

			glm::vec3 mc_center; float mc_r;
			ModelSphere(mc.model, mc_center, mc_r);

			glm::vec3 wc{
				P.x[i] * mc_center.x + P.y[i] * mc_center.y + P.z[i] * mc_center.z + P.w[i],
				P.a[i] * mc_center.x + P.b[i] * mc_center.y + P.c[i] * mc_center.z + P.d[i],
				P.e[i] * mc_center.x + P.f[i] * mc_center.y + P.g[i] * mc_center.z + P.h[i],
			};

			float sx = std::sqrt(P.x[i] * P.x[i] + P.a[i] * P.a[i] + P.e[i] * P.e[i]);
			float sy = std::sqrt(P.y[i] * P.y[i] + P.b[i] * P.b[i] + P.f[i] * P.f[i]);
			float sz = std::sqrt(P.z[i] * P.z[i] + P.c[i] * P.c[i] + P.g[i] * P.g[i]);
			float scale = std::max({ sx, sy, sz });

			spheres.push_back({ e, wc, mc_r * scale });
		});

	std::vector<Contact> contacts;
	for (size_t i = 0; i < spheres.size(); ++i) {
		for (size_t j = i + 1; j < spheres.size(); ++j) {
			float rs = spheres[i].r + spheres[j].r;
			glm::vec3 d = spheres[i].c - spheres[j].c;
			float dist2 = glm::dot(d, d);
			if (dist2 < rs * rs) {
				float dist = std::sqrt(dist2);
				contacts.push_back({ spheres[i].e, spheres[j].e, rs - dist });
			}
		}
	}
	return contacts;
}

}
