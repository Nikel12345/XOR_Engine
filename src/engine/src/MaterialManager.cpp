#include "PCH.h"
#include "MaterialManager.h"
#include "TextureData.h"   // полное определение TextureHandle: нужно для weak_from_this()
#include "TextureManager.h"   // резолв имени текстуры в атлас (CollectSamplerUsage)

MaterialManager::MaterialManager()
{
}

//Material* MaterialManager::CreateMaterial(std::string name, TextureHandle* albedo, TextureHandle* normal, std::vector<ShaderProgram*> shader_programs){
//	auto it = materials.find(name);
//	if (it == materials.end()) {
//		auto data = std::make_unique<Material>();
//		data->shader_programs.insert(shader_programs.begin(), shader_programs.end());
//		data->albedo = albedo;
//		data->normal_texture = normal;
//		materials[name] = std::move(data);
//		return materials[name].get();
//	}
//	else {
//		SDL_Log("Material '%s' already exists.", name.c_str());
//		return it->second.get();
//	}
//}

Material* MaterialManager::CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureName>> textures, std::vector<ShaderName> shaders)
{
	auto it = materials.find(name);
	if (it != materials.end()) {
		SDL_Log("Material '%s' already exists.", name.c_str());
		return it->second.get();
	}

	// Материал держит только имена; резолв в указатели и проверку required_slots уже сделал
	// EngineContext::CreateMaterial. Здесь — чистое хранение (см. правило name-based ссылок).
	auto data = std::make_unique<Material>();
	data->shader_programs = std::move(shaders);
	for (auto& [role, tex_name] : textures) {
		data->textures[role] = std::move(tex_name);
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
		for (auto& [role, tex] : e.textures) m->textures[role] = tex;
		m->shader_programs = e.shaders;
		m->params = e.params;
		m->params_kind = e.params_kind;
		m->dont_save = false;   // пришёл из файла — сохраняемый
		++n;
	}
	return n;
}

void MaterialManager::CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name)
{
	if (!m || !tm) return;
	const auto& handles = tm->GetTextureHandles();
	for (const auto& [role, tex_name] : m->textures) {
		auto it = handles.find(tex_name);   // не GetTextureHandle: тот шумит логом на промах
		if (it == handles.end() || !it->second) continue;   // имя ещё не создано — атлас неизвестен
		TextureAtlas* atlas = it->second->atlas;
		if (!atlas) continue;

		atlas->debug_usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;   // фактическое использование

		// ── Диагностика дыры «намерения» ──
		// Материал сэмплит этот атлас, а его tci про SAMPLER не заявлял. Ресурс создаётся по tci,
		// так что бинд гарантированно упадёт — SDL ударит SDL_assert_release («texture must be
		// created with SAMPLER») и АБОРТИТ процесс, не назвав ни атласа, ни материала. Называем сами.
		//
		// Пофиксить «доклейкой» флага в общем случае НЕЛЬЗЯ: атлас заводится пустым и может быть
		// создан (залит текстурами) за много кадров до того, как на него сошлётся материал — а
		// usage GPU-текстуры после создания неизменяем. Поэтому SAMPLER обязан быть ЗАЯВЛЕН в tci
		// при создании (материальные пресеты это делают; кастомный tci — забота автора).
		if (!(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			const bool already_created = (atlas->texture_binding.texture != nullptr);
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				"USAGE VIOLATION: atlas '%s' is sampled by material '%s' (slot %d, texture '%s') "
				"but was created WITHOUT SDL_GPU_TEXTUREUSAGE_SAMPLER in its tci. %s "
				"Declare SAMPLER in the atlas tci (use a material-atlas preset).",
				atlas->debug_name.c_str(),
				material_name.empty() ? "<unnamed>" : material_name.c_str(),
				static_cast<int>(role), tex_name.c_str(),
				already_created
					? "GPU texture ALREADY CREATED - usage can no longer change, the bind WILL abort."
					: "GPU texture not created yet - still fixable before the next bake.");
		}
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
