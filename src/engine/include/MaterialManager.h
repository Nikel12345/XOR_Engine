#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include "ShaderData.h"
#include "MaterialData.h"


class MaterialManager {
public:
	MaterialManager();
	// Материал хранит ссылки ПО ИМЕНИ (текстуры по роли, sp). Резолв и валидация required_slots —
	// у вызывающего (EngineContext::CreateMaterial): сюда приходят уже готовые имена, менеджер их
	// просто складывает. Пустой/несуществующий на данный момент — допустим (резолвится на сборке батча).
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureName>> textures, std::vector<ShaderName> shaders);
	std::vector<Material*> GetAllMaterials();
	Material* GetMaterial(const std::string& name);
	// Имя→материал (для UI/инспектора). Pointee не const — params можно крутить на лету.
	const std::unordered_map<std::string, std::unique_ptr<Material>>& GetMaterials() const { return materials; }

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