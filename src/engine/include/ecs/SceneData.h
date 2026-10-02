#pragma once
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include "ComponentStorage.h"

using SceneGenerator  = std::function<void()>;
using SceneDestructor = std::function<void()>;

struct SceneData {
    std::unordered_map<Entity, Archetype*> entity_to_archetype;
    std::unordered_map<Entity, size_t> entity_to_index;
    std::map<std::set<std::type_index>, Archetype> archetypes;
    // Обратный индекс иерархии: parent -> прямые дети. Ведут CreateEntity (по ParentComponent) и
    // DeleteEntity; без него каскадное удаление сканировало бы сцену в поисках совпавшего parent.
    std::unordered_map<Entity, std::vector<Entity>> children;
    Entity next_entity_id = 0;
    bool is_active = false;

    std::vector<SceneGenerator>  generators;
    std::vector<SceneDestructor> destructors;

    void clear() {
        archetypes.clear();
        entity_to_archetype.clear();
        entity_to_index.clear();
        children.clear();
        next_entity_id = 0;
    }
};
