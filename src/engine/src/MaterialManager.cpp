#include "PCH.h"
#include "MaterialManager.h"
#include "TextureData.h"
#include "TextureManager.h"
#include "ShaderManager.h"

MaterialManager::MaterialManager()
{
}

Material* MaterialManager::CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, std::vector<TextureId>>> textures, std::vector<ShaderProgramId> shaders, ResourceTag tags)
{
	const MaterialId id = materials.Intern(name);
	if (Material* existing = materials.Get(id)) {
		SDL_Log("Material '%s' already exists.", name.c_str());
		return existing;
	}

	auto data = std::make_unique<Material>();
	data->tags = tags;
	data->shader_programs.reserve(shaders.size());
	for (ShaderProgramId sp_id : shaders) data->shader_programs.push_back(SpBinding{ sp_id, nullptr, {} });
	for (auto& [role, tex_ids] : textures) {
		data->textures[role] = std::move(tex_ids);
	}
	Material* ptr = data.get();
	materials.Put(id, std::move(data));
	return ptr;
}

size_t MaterialManager::ClearSceneMaterials()
{
	size_t removed = 0;
	for (int32_t i = 0; i < materials.Count(); ++i) {
		const Material* m = materials.At(i).object.get();
		if (m && !HasTag(m->tags, ResourceTag::CodeOwned)) removed += materials.Drop(MaterialId{ i }) ? 1 : 0;
	}
	return removed;
}

size_t MaterialManager::LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries, TextureManager* tm, ShaderManager* sm)
{
	size_t n = 0;
	for (const SceneMaterialEntry& e : entries) {
		if (e.name.empty()) continue;
		Material* m = materials.Get(materials.Find(e.name));
		if (m && HasTag(m->tags, ResourceTag::CodeOwned)) {
			SDL_Log("LoadSceneMaterials: '%s' is code-owned - entry skipped", e.name.c_str());
			continue;
		}
		if (!m) m = CreateMaterial(e.name, {}, {});
		if (!m) continue;
		m->textures.clear();
		for (const auto& [role, tex_names] : e.textures) {
			std::vector<TextureId>& ids = m->textures[role];
			ids.reserve(tex_names.size());
			for (const TextureName& tn : tex_names) ids.push_back(tm ? tm->InternTexture(tn) : TextureId{});
		}
		m->shader_programs.clear();
		m->shader_programs.reserve(e.shaders.size());
		for (const SceneShaderEntry& se : e.shaders) {
			SpBinding b;
			b.sp = sm ? sm->InternShaderProgram(se.name) : ShaderProgramId{};
			b.params_type = se.params_type;
			if (!se.params.empty()) b.params = std::make_shared<std::vector<uint8_t>>(se.params);
			m->shader_programs.push_back(std::move(b));
		}
		m->tags = ResourceTag::None;
		++n;
	}
	return n;
}

void MaterialManager::CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name)
{
	if (!m || !tm) return;
	for (const auto& [role, tex_ids] : m->textures)
	for (TextureId tex_id : tex_ids) {
		TextureHandle* h = tm->GetTextureHandle(tex_id);
		if (!h) continue;
		TextureAtlas* atlas = h->atlas;
		if (!atlas) continue;

		// Забейканный атлас без SAMPLER уже не исправить, и бинд убьёт процесс ассертом SDL, не
		// назвав ресурса. Проверка стоит ДО доливки флага ниже — иначе ей нечего было бы ловить.
		if (atlas->texture_binding.texture && !(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				"USAGE VIOLATION: atlas '%s' is sampled by material '%s' (slot %d, texture '%s') "
				"but its GPU texture was ALREADY CREATED without SDL_GPU_TEXTUREUSAGE_SAMPLER - "
				"usage can no longer change, the bind WILL abort. "
				"Declare SAMPLER at atlas creation (use a material-atlas preset).",
				atlas->name.c_str(),
				material_name.empty() ? "<unnamed>" : material_name.c_str(),
				static_cast<int>(role), tm->TextureNameOf(tex_id).c_str());
		}

		atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
	}
}

Material* MaterialManager::GetMaterial(const std::string& name)
{
	Material* m = materials.Get(materials.Find(name));
	if (!m) SDL_Log("Material '%s' not found.", name.c_str());
	return m;
}

MaterialManager::~MaterialManager()
{
}
