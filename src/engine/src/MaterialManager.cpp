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
