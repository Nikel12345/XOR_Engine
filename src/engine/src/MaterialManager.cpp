#include "PCH.h"
#include "MaterialManager.h"
#include "TextureData.h"
#include "TextureManager.h"

MaterialManager::MaterialManager()
{
}

Material* MaterialManager::CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, std::vector<TextureName>>> textures, std::vector<ShaderName> shaders)
{
	auto it = materials.find(name);
	if (it != materials.end()) {
		SDL_Log("Material '%s' already exists.", name.c_str());
		return it->second.get();
	}

	// Материал держит только имена; резолв в указатели и проверку required_slots уже сделал
	// EngineContext::CreateMaterial. Здесь — чистое хранение (см. правило name-based ссылок).
	auto data = std::make_unique<Material>();
	// Ячейка на каждую sp; данных у неё пока нет (их кладёт SetMaterialParams по имени sp).
	data->shader_programs.reserve(shaders.size());
	for (ShaderName& sp_name : shaders) data->shader_programs.push_back(SpBinding{ std::move(sp_name), nullptr, {} });
	for (auto& [role, tex_names] : textures) {
		data->textures[role] = std::move(tex_names);
	}
	materials[name] = std::move(data);
	return materials[name].get();
}

size_t MaterialManager::LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries)
{
	size_t n = 0;
	for (const SceneMaterialEntry& e : entries) {
		if (e.name.empty()) continue;
		// Обновление В МЕСТЕ (сохраняем адрес Material — если на него уже кто-то ссылается): если
		// нет — создаём пустой. Затем переливаем все поля из записи манифеста.
		auto it = materials.find(e.name);
		Material* m = (it != materials.end()) ? it->second.get()
		            : CreateMaterial(e.name, {}, {});   // пустой под этим именем
		if (!m) continue;
		m->textures.clear();
		for (auto& [role, tex] : e.textures) m->textures[role] = tex;   // список вариантов целиком
		// Ячейки пересобираем целиком: блобы прежних sp уходят на кладбище материала, а не в free —
		// на их адреса может смотреть слепок рендера (см. Material::retired_params).
		for (SpBinding& old : m->shader_programs)
			if (old.params) m->retired_params.push_back(std::move(old.params));
		m->shader_programs.clear();
		m->shader_programs.reserve(e.shaders.size());
		for (const SceneShaderEntry& se : e.shaders) {
			SpBinding b;
			b.sp = se.name;
			b.params_type = se.params_type;
			if (!se.params.empty()) b.params = std::make_unique<std::vector<uint8_t>>(se.params);
			m->shader_programs.push_back(std::move(b));
		}
		m->dont_save = false;   // пришёл из файла — сохраняемый
		++n;
	}
	return n;
}

void MaterialManager::CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name)
{
	if (!m || !tm) return;
	const auto& handles = tm->GetTextureHandles();
	// ВСЕ варианты слота, а не только [0]: переключить можно любой, значит сэмплиться будет любой.
	for (const auto& [role, tex_names] : m->textures)
	for (const TextureName& tex_name : tex_names) {
		auto it = handles.find(tex_name);   // не GetTextureHandle: тот шумит логом на промах
		if (it == handles.end() || !it->second) continue;   // имя ещё не создано — атлас неизвестен
		TextureAtlas* atlas = it->second->atlas;
		if (!atlas) continue;

		// ── Диагностика ОПОЗДАВШЕЙ декларации (проверка ДО доливки флага) ──
		// GPU-текстура уже создана без SAMPLER — usage неизменяем, бинд гарантированно упадёт:
		// SDL ударит SDL_assert_release («texture must be created with SAMPLER») и АБОРТИТ процесс,
		// не назвав ни атласа, ни материала. Называем сами. Так бывает, когда атлас забейкали
		// (залили текстурами) за много кадров до того, как на него сослался материал, — потому
		// SAMPLER и обязан быть ЗАЯВЛЕН намерением при создании (материальные пресеты это делают).
		// Не созданный атлас чинится самой декларацией ниже — бейк создаст его уже с флагом.
		if (atlas->texture_binding.texture && !(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				"USAGE VIOLATION: atlas '%s' is sampled by material '%s' (slot %d, texture '%s') "
				"but its GPU texture was ALREADY CREATED without SDL_GPU_TEXTUREUSAGE_SAMPLER - "
				"usage can no longer change, the bind WILL abort. "
				"Declare SAMPLER at atlas creation (use a material-atlas preset).",
				atlas->debug_name.c_str(),
				material_name.empty() ? "<unnamed>" : material_name.c_str(),
				static_cast<int>(role), tex_name.c_str());
		}

		atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;   // декларация: слот материала = сэмплер
	}
}

std::vector<Material*> MaterialManager::GetAllMaterials()
{
    std::vector<Material*> result;
	result.reserve(materials.size());
    for (auto& [name, material] : materials) {
        result.push_back(material.get());
	}
	return result;
}

Material* MaterialManager::GetMaterial(const std::string& name)
{
	auto it = materials.find(name);
	if (it != materials.end()) {
		return it->second.get();
	}
	SDL_Log("Material '%s' not found.", name.c_str());
	return nullptr;
}

MaterialManager::~MaterialManager()
{
}
