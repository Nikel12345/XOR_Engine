#pragma once
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include "ComponentStorage.h"   // нужен Archetype/Entity, а не конкретные компоненты

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

    // Генераторы — функции, восстанавливающие ПРОИЗВОДНЫЕ сущности сцены из её авторских
    // данных при настройке (запускаются в LoadScene). Регистрируются ОДНОКРАТНО на уже
    // существующую сцену (паттерн «ресурс создан до использования»: сцена — через
    // CreateScene, Load её лишь наполняет). vector, не map: повторной регистрации нет
    // (переживают clear), поэтому дубликатов не возникает и ключевой доступ не нужен.
    std::vector<std::function<void()>> generators;

    // Сбрасывает СОДЕРЖИМОЕ сцены (сущности/иерархию), но НЕ генераторы: они навешены
    // один раз и должны пережить clear, чтобы перезагрузка их снова запустила.
    void clear() {
        archetypes.clear();
        entity_to_archetype.clear();
        entity_to_index.clear();
        children.clear();
        next_entity_id = 0;
    }
};
