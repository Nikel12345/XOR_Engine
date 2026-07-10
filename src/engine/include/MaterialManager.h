#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include <string>
#include "MaterialData.h"   // Material + TextureSlotRole (через ShaderTypes.h)

// Запись манифеста материалов сцены (materials.json): всё для пересоздания. params — сырой блоб
// (layout задаёт автор шейдера, движок видит только байты), params_kind — тег UI-разбора.
struct SceneMaterialEntry {
	std::string name;
	std::vector<std::pair<TextureSlotRole, TextureName>> textures;
	std::vector<ShaderName> shaders;
	MaterialParamsKind      params_kind = MaterialParamsKind::None;
	std::vector<uint8_t>    params;
};

class MaterialManager {
public:
	MaterialManager();
	// Материал хранит ссылки ПО ИМЕНИ (текстуры по роли, sp). Резолв и валидация required_slots —
	// у вызывающего (EngineContext::CreateMaterial): сюда приходят уже готовые имена, менеджер их
	// просто складывает. Пустой/несуществующий на данный момент — допустим (резолвится на сборке батча).
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureName>> textures, std::vector<ShaderName> shaders);

	// Merge-upsert материалов из манифеста (см. SceneMaterialEntry). Существующий обновляется
	// В МЕСТЕ (указатель у энтити жив — MaterialComponent.materials перепривяжется на пересборке,
	// но и так тот же адрес), новый создаётся. params/params_kind проставляются напрямую. Материалы
	// вне манифеста не трогаются. Возвращает число обработанных.
	size_t LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries);

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

	// Тип-безопасная упаковка per-material факторов в Material::params (непрозрачный блоб).
	// T должен совпадать по размеру/раскладке с cbuffer MaterialBlock в шейдере.
	template<class T>
	void SetMaterialParams(Material* m, const T& p) {
		if (!m) return;
		m->params.resize(sizeof(T));
		std::memcpy(m->params.data(), &p, sizeof(T));
		m->params_kind = T::kind;   // тег для UI-разбора (рендер его не читает)
	}

	~MaterialManager();
private:
	std::unordered_map<std::string, std::unique_ptr<Material>> materials;
};