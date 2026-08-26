#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include <string>
#include "MaterialData.h"
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
	std::vector<std::pair<TextureSlotRole, TextureName>> textures;
	std::vector<SceneShaderEntry> shaders;
};

class MaterialManager {
public:
	MaterialManager();
	// Материал хранит ссылки ПО ИМЕНИ (текстуры по роли, sp). Резолв и валидация required_slots —
	// у вызывающего (EngineContext::CreateMaterial): сюда приходят уже готовые имена, менеджер их
	// просто складывает. Пустой/несуществующий на данный момент — допустим (резолвится на сборке батча).
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureName>> textures, std::vector<ShaderName> shaders);

	// Merge-upsert материалов из манифеста (см. SceneMaterialEntry). Существующий обновляется
	// В МЕСТЕ, новый создаётся. params/params_type проставляются напрямую. Материалы вне
	// манифеста не трогаются. Возвращает число обработанных.
	size_t LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries);

	// Сбор usage-флагов. Слот материала — это ФРАГМЕНТНЫЙ СЭМПЛЕР по определению (TextureSlotRole →
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

	std::vector<Material*> GetAllMaterials();
	Material* GetMaterial(const std::string& name);
	// Имя→материал (для UI/инспектора). Pointee не const — params можно крутить на лету.
	const std::unordered_map<std::string, std::unique_ptr<Material>>& GetMaterials() const { return materials; }

	// Переименование = ре-кей узла словаря (сам Material сохраняется; путь «delete+create» на уровне
	// ключа). false, если имена совпали / новое занято / старого нет. Ссылки по СТАРОМУ имени
	// (MaterialComponent.names) после этого не резолвятся — переименовывай до назначения материала.
	bool RenameMaterial(const std::string& oldName, const std::string& newName) {
		if (oldName == newName || materials.count(newName)) return false;
		auto node = materials.extract(oldName);
		if (node.empty()) return false;
		node.key() = newName;
		materials.insert(std::move(node));
		return true;
	}

	// Тип-безопасная упаковка per-sp факторов в блоб ячейки (непрозрачные байты для рендера).
	// T должен совпадать по размеру/раскладке с cbuffer MaterialBlock ИМЕННО ЭТОЙ sp И быть
	// зарегистрирован в ParamsSpecRegistry (оттуда берётся имя типа для тега).
	template<class T>
	void SetMaterialParams(Material* m, const ShaderName& sp_name, const T& p) { ::SetMaterialParams(m, sp_name, p); }

	~MaterialManager();
private:
	std::unordered_map<std::string, std::unique_ptr<Material>> materials;
};