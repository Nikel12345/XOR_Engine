#pragma once
#include <map>
#include <set>
#include <typeindex>
#include "BaseComponents.h"

struct SceneData {
    std::unordered_map<Entity, Archetype*> entity_to_archetype;
    std::unordered_map<Entity, size_t> entity_to_index;
    std::map<std::set<std::type_index>, Archetype> archetypes;
    Entity next_entity_id = 0;
    bool is_active = true;

    void clear() {
        archetypes.clear();
        entity_to_archetype.clear();
        entity_to_index.clear();
        next_entity_id = 0;
    }
};
