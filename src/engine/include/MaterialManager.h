#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include "ShaderData.h"
#include "MaterialData.h"


class MaterialManager {
public:
	MaterialManager();
	// ����� ��� TextureSlotRole, TextureHandle* ������ ��������� � ������ required_slots � ������ ShaderProgram* �� shader_programs. ������� �� �����, �� ��� ���� �� required_slots ������ ���� ������������ � textures.
	// TextureSlotRole, TextureHandle* count must match the number of required_slots in each ShaderProgram* in shader_programs. The order does not matter, but all roles from required_slots must be represented in textures.
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureHandle*>>& textures, std::vector<ShaderProgram*>& shader_programs);
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