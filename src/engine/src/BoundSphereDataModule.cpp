#include "PCH.h"
#include "BoundSphereDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "ModelData.h"

BoundSphereDataModule::BoundSphereDataModule()
{
	for (uint64_t& r : last_revision) r = ~0ull;
}

// Минимальная сфера, содержащая обе (стандартное объединение).
static glm::vec4 UnionSpheres(const glm::vec4& a, const glm::vec4& b)
{
	glm::vec3 d = glm::vec3(b) - glm::vec3(a);
	float dist = glm::length(d);
	if (a.w >= dist + b.w) return a;   // b целиком внутри a
	if (b.w >= dist + a.w) return b;   // a целиком внутри b
	float R = (dist + a.w + b.w) * 0.5f;
	glm::vec3 c = glm::vec3(a) + d * ((R - a.w) / dist);   // dist > 0: случаи вложенности отсечены выше
	return glm::vec4(c, R);
}

// Сфера модели = объединение сфер её сабмешей. Нет модели/сабмешей → w = -1
// («нет геометрии»; culling-шейдер считает такую строку всегда видимой).
static glm::vec4 ModelSphere(const ModelData* model)
{
	if (!model || model->submeshes.empty()) return glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
	glm::vec4 sphere = model->submeshes[0].sphere;
	for (size_t i = 1; i < model->submeshes.size(); ++i)
		sphere = UnionSpheres(sphere, model->submeshes[i].sphere);
	return sphere;
}

uint32_t BoundSphereDataModule::CalculateSphereSize(ObjectManager* om, uint64_t revision, uint8_t slot)
{
	if (revision == last_revision[slot]) return 0;
	last_revision[slot] = revision;

	uint32_t rows = 0;
	om->ForEachArchetype<Positions, DrawComponent>(om->GetActiveScene(),
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<DrawComponent, void>*)
	{
		rows += safe_u32(posArr->size());
	});

	total_size = rows * sizeof(glm::vec4);
	return total_size;
}

void BoundSphereDataModule::StoreSpheres(BufferManager* bm, UploadTask* task, ObjectManager* om)
{
	SceneData* scene = om->GetActiveScene();
	if (!scene) return;

	// Прямой обход scene->archetypes с тем же отбором, что RecalculateInstanceOffsets /
	// TransformDataModule — порядок строк обязан совпадать с буфером трансформов.
	for (auto& [sig, arch] : scene->archetypes) {
		if (!arch.get_array<DrawComponent>() || !arch.get_array<Positions>()) continue;

		const size_t n = arch.entities.size();
		if (n == 0) continue;

		auto* model_arr = arch.get_array<ModelComponent>();
		glm::vec4* dst = static_cast<glm::vec4*>(
			bm->AcquireTransferWritePtr(task, safe_u32(n * sizeof(glm::vec4))));
		if (!dst) return;

		for (size_t i = 0; i < n; ++i)
			dst[i] = model_arr ? ModelSphere((*model_arr)[i].model)
			                   : glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
	}
}
