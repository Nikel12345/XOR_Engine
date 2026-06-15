#include "PCH.h"
#include "ObjectManager.h"
#include "RenderManager.h"
#include "PipeManager.h"
#include "TextureData.h"
#include "ModelData.h"

SceneData* ObjectManager::CreateScene(const SceneName& name) {
    auto [it, inserted] = scenes_data.emplace(name, std::make_unique<SceneData>());
    return it->second.get();
}

SceneData* ObjectManager::operator[](const std::string& name) {
    auto it = scenes_data.find(name);
    if (it != scenes_data.end()){
		return it->second.get();
	}
    SDL_Log("Scene '%s' not found!", name.c_str());

    return nullptr;
}

void ObjectManager::DeleteEntity(const SceneName& name, Entity e) {
    DeleteEntity(GetScene(name), e);
}

void ObjectManager::DeleteEntity(SceneData* scene, Entity e) {
    if (!scene) { SDL_Log("DeleteEntity: null scene"); return; }

    auto arch_it = scene->entity_to_archetype.find(e);
    if (arch_it == scene->entity_to_archetype.end()) {
        SDL_Log("DeleteEntity: entity %u not present", e);
        return;
    }
    Archetype* arch = arch_it->second;

    auto idx_it = scene->entity_to_index.find(e);
    SDL_assert(idx_it != scene->entity_to_index.end());
    const size_t i = idx_it->second;
    const size_t last = arch->entities.size() - 1;

    arch->swap_remove(i);

    // swap-pop вектора сущностей + индекс переехавшего
    if (i != last) {
        Entity moved = arch->entities[last];
        arch->entities[i] = moved;
        scene->entity_to_index[moved] = i;
    }
    arch->entities.pop_back();

    // выкинуть удаляемого из карт
    scene->entity_to_index.erase(e);
    scene->entity_to_archetype.erase(e);

    // НЕ ставим dirty_batches — удаление идёт инкрементально через e_t_d, а не ребилдом.
    // Но трансформ-буфер ужался: помечаем, чтобы TransformDataModule пересчитал размер.
    dirty_entity = true;
}

void ObjectManager::SetSceneState(const SceneName& scene_name, bool is_active)
{
    auto scene = (*this)[scene_name];
    if (scene) {
        scene->is_active = is_active;
    }
    else {
        SDL_Log("Scene '%s' not found!", scene_name.c_str());
	}
}

SceneData* ObjectManager::GetActiveScene()
{
    for (auto& [name, scene] : scenes_data) {
        if (scene->is_active)
            return scene.get();
    }
    SDL_Log("No active scene found!");
    return nullptr; // не найдено
}

SceneName ObjectManager::GetActiveSceneName()
{
    for (auto& [name, scene] : scenes_data) {
        if (scene->is_active)
            return name;
    }
    SDL_Log("No active scene found!");
    return {};
}

SceneData* ObjectManager::GetScene(const SceneName& name)
{
    auto it = scenes_data.find(name);
    if (it != scenes_data.end()) {
        return it->second.get();
    }
    else {
        SDL_Log("Scene '%s' not found!", name.c_str());
        return nullptr;
	}
}

