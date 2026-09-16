#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include <string>
#include "MaterialData.h"
#include "ResourceRegistry.h"
#include "ParamsSpec.h"

class TextureManager;   // только в сигнатуре CollectSamplerUsage — передаётся на вызове

// Запись манифеста материалов сцены (materials.json): всё для пересоздания. params — блоб,
// уже собранный по схеме типа params_type (разбор json ↔ поля — в Engine_Scene), и он
// СВОЙ У КАЖДОЙ sp: адресат данных — программа, а не материал (см. SpBinding).
struct SceneShaderEntry {
	ShaderName           name;
	std::string          params_type;   // имя типа в ParamsSpecRegistry; пусто = без params
	std::vector<uint8_t> params;
};
struct SceneMaterialEntry {
	std::string name;
	// Слот = СПИСОК имён ([0] — дефолт, дальше варианты), как в Material::textures.
	std::vector<std::pair<TextureSlotRole, std::vector<TextureName>>> textures;
	std::vector<SceneShaderEntry> shaders;
};

struct MaterialCell { std::string name; std::unique_ptr<Material> object; };
using MaterialRegistry = ResourceRegistry<MaterialCell, MaterialId>;

class MaterialManager {
public:
	MaterialManager();
	// Материал хранит ссылки: текстуры — по id ячейки, sp — по имени. Перевод имён в id и валидация
	// required_slots — у вызывающего (EngineContext::CreateMaterial): сюда приходят уже готовые
	// ссылки, менеджер их просто складывает. Пустая/неразрешимая сейчас — допустима (резолв на сборке батча).
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, std::vector<TextureId>>> textures, std::vector<ShaderName> shaders);

	// Merge-upsert материалов из манифеста (см. SceneMaterialEntry). Существующий обновляется
	// В МЕСТЕ, новый создаётся. params/params_type проставляются напрямую. Материалы вне
	// манифеста не трогаются. Возвращает число обработанных.
	size_t LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries, TextureManager* tm);

	size_t ClearSceneMaterials();

	// Сбор usage-флагов. Обходит ВСЕ варианты слота, а не только дефолт: пропущенный вариант
	// сломается не здесь, а на первом кадре, где его включили (атлас уже создан без SAMPLER —
	// см. WARNINGS.md про поздний usage-флаг).
	// Слот материала — это ФРАГМЕНТНЫЙ СЭМПЛЕР по определению (TextureSlotRole →
	// SDL_BindGPUFragmentSamplers, см. ShaderTypes.h), поэтому атлас каждой текстуры материала
	// обязан иметь SAMPLER. Резолв имени в атлас — через TextureManager, переданный НА ВЫЗОВЕ
	// (менеджер не хранит указателей на другие менеджеры; связку держит EngineContext).
	// Зовётся везде, где материалу назначается текстура: создание, загрузка манифеста, правка из UI.
	// Storage-текстур у материала нет и не предвидится: атласная механика (UVL + мипы + фильтрация)
	// осмысленна только при сэмплировании, а пассовым storage-ресурсам место в проходе, не в материале.
	//
	// ЗАОДНО ЛОВИТ НАРУШЕНИЕ: атлас, который материал сэмплит, а его tci SAMPLER не объявлял. Такой
	// бинд гарантированно упадёт (SDL_assert + abort, БЕЗ имени ресурса) — здесь он называется
	// поимённо, до краша. material_name — только для этого сообщения.
	void CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name);

	Material* GetMaterial(const std::string& name);
	Material* GetMaterial(MaterialId id) const        { return materials.Get(id); }
	MaterialId         MaterialIdOf(const std::string& name) const { return materials.Find(name); }
	MaterialId         InternMaterial(const std::string& name)     { return materials.Intern(name); }
	const std::string& MaterialNameOf(MaterialId id) const         { return materials.NameOf(id); }
	// Реестр материалов (для UI/инспектора). Pointee не const — params можно крутить на лету.
	const MaterialRegistry& Materials() const { return materials; }

	// Имя — поле ЯЧЕЙКИ; ссылающиеся держат её id, поэтому переименование их не касается.
	// false, если имена совпали / новое занято / старого нет.
	bool RenameMaterial(MaterialId id, const std::string& newName) {
		if (!materials.Get(id) || newName.empty()) return false;
		const MaterialId taken = materials.Find(newName);
		if (taken && taken != id) return false;
		materials.Rename(id, newName);
		return true;
	}

	// Тип-безопасная упаковка per-sp факторов в блоб ячейки (непрозрачные байты для рендера).
	// T должен совпадать по размеру/раскладке с cbuffer MaterialBlock ИМЕННО ЭТОЙ sp И быть
	// зарегистрирован в ParamsSpecRegistry (оттуда берётся имя типа для тега).
	template<class T>
	void SetMaterialParams(Material* m, const ShaderName& sp_name, const T& p) { ::SetMaterialParams(m, sp_name, p); }

	~MaterialManager();
private:
	MaterialRegistry materials;
};