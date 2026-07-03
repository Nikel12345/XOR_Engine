#include "PCH.h"
#include "MaterialManager.h"
#include "TextureData.h"   // полное определение TextureHandle: нужно для weak_from_this()

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

Material* MaterialManager::CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, TextureHandle*>>& textures, std::vector<ShaderProgram*>& shader_programs)
{
	auto it = materials.find(name);
	if (it != materials.end()) {
		SDL_Log("Material '%s' already exists.", name.c_str());
		return it->second.get();
	}

	for (auto shader_program : shader_programs) {
		if (!shader_program) {
			SDL_Log("MaterialManager::Creating material with non existing shader program");
			continue;
		}
		// ��������� ��� ��� required_slots ������� ����������� textures
		for (const auto& required_role : shader_program->required_slots) {
			bool found = false;
			for (const auto& [role, handle] : textures) {
				if (role == required_role) {
					found = true;
					break;
				}
			}
			if (!found) {
				SDL_Log("Material '%s': missing texture for required slot %d",
					name.c_str(), static_cast<int>(required_role));
				return nullptr; // ��� assert, ��� throw
			}
		}
	}
	auto data = std::make_unique<Material>();
	data->shader_programs.insert(shader_programs.begin(), shader_programs.end());
	for (const auto& [role, handle] : textures) {
		// Сырой хэндл → weak_ptr (владелец shared_ptr — TextureManager::handles_data). Пустой weak
		// для отсутствующей текстуры; удаление хэндла позже сделает weak протухшим → dummy в батче.
		data->textures[role] = handle ? handle->weak_from_this() : std::weak_ptr<TextureHandle>{};
	}
	materials[name] = std::move(data);
	return materials[name].get();
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
