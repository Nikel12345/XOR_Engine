#pragma once
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include "ComponentStorage.h"

struct SceneData {
    std::unordered_map<Entity, Archetype*> entity_to_archetype;
    std::unordered_map<Entity, size_t> entity_to_index;
    std::map<std::set<std::type_index>, Archetype> archetypes;
    // Обратный индекс иерархии: parent -> прямые дети. Ведут CreateEntity (по ParentComponent) и
    // DeleteEntity; без него каскадное удаление сканировало бы сцену в поисках совпавшего parent.
    std::unordered_map<Entity, std::vector<Entity>> children;
    Entity next_entity_id = 0;
    bool is_active = true;

    // Восстанавливают ПРОИЗВОДНЫЕ сущности сцены (те, что с GeneratedComponent и потому не
    // сохранены) — запускаются после загрузки. Регистрируются один раз на живую сцену и
    // переживают clear, иначе перезагрузка осталась бы без них.
    std::vector<std::function<void()>> generators;

    void clear() {
        archetypes.clear();
        entity_to_archetype.clear();
        entity_to_index.clear();
        children.clear();
        next_entity_id = 0;
    }
};
