#include "PCH.h"
#include "BaseComponents.h"
#include "BoundSphereDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "ModelManager.h"
#include "ModelData.h"

BoundSphereDataModule::BoundSphereDataModule()
{
	for (uint64_t& r : last_revision) r = ~0ull;
}

static glm::vec4 UnionSpheres(const glm::vec4& a, const glm::vec4& b)
{
	glm::vec3 d = glm::vec3(b) - glm::vec3(a);
	float dist = glm::length(d);
	if (a.w >= dist + b.w) return a;
	if (b.w >= dist + a.w) return b;
	float R = (dist + a.w + b.w) * 0.5f;
	glm::vec3 c = glm::vec3(a) + d * ((R - a.w) / dist);   // dist > 0: случаи вложенности отсечены выше
	return glm::vec4(c, R);
}

// w = -1 читается шейдером отсева как «геометрии нет»: такая строка скаттерится безусловно.
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

void BoundSphereDataModule::StoreSpheres(BufferManager* bm, UploadTask* task, ObjectManager* om, ModelManager* mm)
{
	SceneData* scene = om->GetActiveScene();
	if (!scene) return;

	// Память на одно последнее имя: без неё это поиск в словаре на КАЖДУЮ строку, а на 1М объектов
	// он дороже всего остального заполнения. Указатель на строку компонента жив до конца вызова —
	// ECS в этом проходе не мутируется.
	const std::string* memo_name = nullptr;
	glm::vec4 memo_sphere(0.0f, 0.0f, 0.0f, -1.0f);
	auto sphere_of = [&](const std::string& name) -> glm::vec4 {
		if (memo_name && *memo_name == name) return memo_sphere;
		const ModelData* model = mm ? mm->FindModel(name) : nullptr;
		memo_sphere = ModelSphere(model);
		memo_name   = &name;
		return memo_sphere;
	};

	// Отбор и порядок обязаны совпадать с RecalculateInstanceOffsets и TransformDataModule.
	for (auto& [sig, arch] : scene->archetypes) {
		if (!arch.get_array<DrawComponent>() || !arch.get_array<Positions>()) continue;

		const size_t n = arch.entities.size();
		if (n == 0) continue;

		// UI живёт в NDC, и мировой фрустум мис-каллил бы его — уходит вырожденной сферой.
		const bool is_ui = arch.get_array<UIComponent>() != nullptr;

		auto* model_arr = arch.get_array<ModelComponent>();
		glm::vec4* dst = static_cast<glm::vec4*>(
			bm->AcquireTransferWritePtr(task, safe_u32(n * sizeof(glm::vec4))));
		if (!dst) return;

		for (size_t i = 0; i < n; ++i)
			dst[i] = (is_ui || !model_arr) ? glm::vec4(0.0f, 0.0f, 0.0f, -1.0f)
			                               : sphere_of((*model_arr)[i].name);
	}
}
