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

	// Перевод имён в ссылки и проверку required_slots уже сделал EngineContext::CreateMaterial.
	// Здесь — чистое хранение.
	auto data = std::make_unique<Material>();
	data->tags = tags;
	// Ячейка на каждую sp; данных у неё пока нет (их кладёт SetMaterialParams по имени sp).
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
		if (m && !HasTag(m->tags, ResourceTag::CodeOwned)) removed += materials.Clear(MaterialId{ i }) ? 1 : 0;
	}
	return removed;
}

size_t MaterialManager::LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries, TextureManager* tm, ShaderManager* sm)
{
	size_t n = 0;
	for (const SceneMaterialEntry& e : entries) {
		if (e.name.empty()) continue;
		// Обновление В МЕСТЕ (сохраняем адрес Material — если на него уже кто-то ссылается): если
		// нет — создаём пустой. Затем переливаем все поля из записи манифеста.
		Material* m = materials.Get(materials.Find(e.name));
		if (!m) m = CreateMaterial(e.name, {}, {});   // пустой под этим именем
		if (!m) continue;
		m->textures.clear();
		for (const auto& [role, tex_names] : e.textures) {
			std::vector<TextureId>& ids = m->textures[role];   // список вариантов целиком
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
		m->tags = ResourceTag::None;   // пришёл из файла — сохраняемый
		++n;
	}
	return n;
}

void MaterialManager::CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name)
{
	if (!m || !tm) return;
	// ВСЕ варианты слота, а не только [0]: переключить можно любой, значит сэмплиться будет любой.
	for (const auto& [role, tex_ids] : m->textures)
	for (TextureId tex_id : tex_ids) {
		TextureHandle* h = tm->GetTextureHandle(tex_id);
		if (!h) continue;   // текстуру ещё не создали — атлас неизвестен
		TextureAtlas* atlas = h->atlas;
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
				atlas->name.c_str(),
				material_name.empty() ? "<unnamed>" : material_name.c_str(),
				static_cast<int>(role), tm->TextureNameOf(tex_id).c_str());
		}

		atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;   // декларация: слот материала = сэмплер
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
