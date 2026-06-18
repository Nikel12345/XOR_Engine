#pragma once
#include <map>
#include <set>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include "BaseComponents.h"

struct SceneData {
    std::unordered_map<Entity, Archetype*> entity_to_archetype;
    std::unordered_map<Entity, size_t> entity_to_index;
    std::map<std::set<std::type_index>, Archetype> archetypes;
    // Обратный индекс иерархии: parent -> его прямые дети. Ведётся в CreateEntity
    // (по ParentComponent) и DeleteEntity. Нужен, чтобы каскадно удалять детей за
    // O(1) на каждого, не сканируя всю сцену в поисках совпадающего parent.
    std::unordered_map<Entity, std::vector<Entity>> children;
    Entity next_entity_id = 0;
    bool is_active = true;

    void clear() {
        archetypes.clear();
        entity_to_archetype.clear();
        entity_to_index.clear();
        children.clear();
        next_entity_id = 0;
    }
};
