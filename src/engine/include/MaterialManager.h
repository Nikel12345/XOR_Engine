#pragma once
#include <unordered_map>
#include <vector>
#include <cstring>
#include <string>
#include "MaterialData.h"
#include "ResourceRegistry.h"
#include "ParamsSpec.h"

class TextureManager;   // только в сигнатуре CollectSamplerUsage — передаётся на вызове
class ShaderManager;

struct SceneShaderEntry {
	ShaderName           name;
	std::string          params_type;
	std::vector<uint8_t> params;
};
struct SceneMaterialEntry {
	std::string name;
	std::vector<std::pair<TextureSlotRole, std::vector<TextureName>>> textures;
	std::vector<SceneShaderEntry> shaders;
};

struct MaterialCell { std::string name; std::unique_ptr<Material> object; };
using MaterialRegistry = ResourceRegistry<MaterialCell, MaterialId>;

class MaterialManager {
public:
	MaterialManager();
	Material* CreateMaterial(std::string name, std::vector<std::pair<TextureSlotRole, std::vector<TextureId>>> textures, std::vector<ShaderProgramId> shaders, ResourceTag tags = ResourceTag::None);

	size_t LoadSceneMaterials(const std::vector<SceneMaterialEntry>& entries, TextureManager* tm, ShaderManager* sm);

	size_t ClearSceneMaterials();

	// Звать везде, где материалу назначается текстура: атлас обязан получить SAMPLER до бейка,
	// иначе бинд упадёт много позже (см. WARNINGS.md про поздний usage-флаг).
	void CollectSamplerUsage(const Material* m, TextureManager* tm, const std::string& material_name);

	Material* GetMaterial(const std::string& name);
	Material* GetMaterial(MaterialId id) const        { return materials.Get(id); }
	MaterialId         MaterialIdOf(const std::string& name) const { return materials.Find(name); }
	MaterialId         InternMaterial(const std::string& name)     { return materials.Intern(name); }
	const std::string& MaterialNameOf(MaterialId id) const         { return materials.NameOf(id); }
	const MaterialRegistry& Materials() const { return materials; }

	bool RenameMaterial(MaterialId id, const std::string& newName) {
		if (!materials.Get(id) || newName.empty()) return false;
		return materials.Rename(id, newName);
	}

	~MaterialManager();
private:
	MaterialRegistry materials;
};