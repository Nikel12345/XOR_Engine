#pragma once
#include "ObjectManager.h"
#include <type_traits>


template<typename... Components>
Entity ObjectManager::CreateEntity(const std::string& scene_name, Components&&... comps) {
    SceneData* scene = (*this)[scene_name];
    if (!scene) {
		SDL_Log("Create entity failed: scene '%s' not found!", scene_name.c_str());
        return static_cast<Entity>(-1);
    }

    std::set<std::type_index> sig;
    (..., (
        [&] {
            using T = std::decay_t<Components>;
            if constexpr (has_related_soa<T>::value)
                sig.insert(std::type_index(typeid(typename T::related_soa)));
            else
                sig.insert(std::type_index(typeid(T)));
        }()
            ));

    Archetype& arch = scene->archetypes[sig];
    Entity e = scene->next_entity_id++;
    arch.entities.push_back(e);

    (..., (
        [&] {
            using T = std::decay_t<Components>;
            if constexpr (has_related_soa<T>::value)
                arch.ensure_component<typename T::related_soa>();
            else
                arch.ensure_component<T>();
        }()
            ));

    // Снимаем parent ДО add_components (дальше comps форвардятся в хранилище).
    bool has_parent = false;
    Entity parent_id = 0;
    (..., (
        [&] {
            using T = std::decay_t<decltype(comps)>;
            if constexpr (std::is_same_v<T, ParentComponent>) {
                has_parent = true;
                parent_id = comps.parent;
            }
        }()
            ));

    add_components(arch, std::forward<Components>(comps)...);
    scene->entity_to_archetype[e] = &arch;
    scene->entity_to_index[e] = arch.entities.size() - 1;

    // Регистрируем ребёнка в обратном индексе иерархии (для каскадного удаления).
    if (has_parent)
        scene->children[parent_id].push_back(e);

	dirty_entity = true;

    return e;
}

template<typename... Ts, typename Fn>
void ObjectManager::ForEachArchetype(SceneData* scene, Fn&& fn) {
    if (!scene) return;
    for (auto& [sig, arch] : scene->archetypes) {
        auto arrs = std::tuple{ arch.get_array<Ts>()... };
        bool all_present = std::apply([](auto*... arr) {
            return (... && (arr != nullptr));
            }, arrs);
        if (!all_present) continue;
        std::apply([&](auto*... arr) {
            fn(arr...);
            }, arrs);
    }
}



template<typename T>
std::enable_if_t<!is_soa<T>::value, T&>
make_param(ComponentArray<T>* arr, size_t i) {
    return (*arr)[i];  // AoS
}

template<typename T>
std::enable_if_t<is_soa<T>::value, SoAElement<T>>
make_param(ComponentArray<T>* arr, size_t i) {
    return SoAElement<T>{ &arr->data, i };  // SoA
}


template<typename... Ts, typename Fn>
void ObjectManager::ForEach(SceneData* scene, Fn&& fn) {
    if (!scene) {
        SDL_Log("ForEach called on null scene!");
        return;
    }

    auto& f = fn;

    constexpr bool all_soa = (is_soa<Ts>::value && ...);

    constexpr bool wants_entity =
        std::is_invocable_v<std::decay_t<Fn>, Entity, foreach_arg_t<Ts>...>;


    for (auto& [sig, arch] : scene->archetypes) {
        auto arrs = std::tuple{ arch.get_array<Ts>()... };

        bool all_present = std::apply([](auto*... arr) {
            return (... && (arr != nullptr));
        }, arrs);
        if (!all_present) continue;

        if constexpr (all_soa && !wants_entity) {
            std::apply([&](auto*... arr) {
                f((arr->data)...);
            }, arrs);
        }
        else {
            const size_t count = arch.entities.size();

            for (size_t i = 0; i < count; ++i) {
                Entity e = arch.entities[i];

                std::apply([&](auto*... arr) {
                    if constexpr (wants_entity) {
                        f(e, make_param<Ts>(arr, i)...);
                    }
                    else {
                        f(make_param<Ts>(arr, i)...);
                    }
                }, arrs);
            }
        }
    }
}

template<typename... Components>
void ObjectManager::add_components(Archetype& arch, Components&&... comps) {
    (..., (
        [&] {
            using T = std::decay_t<decltype(comps)>;
            if constexpr (has_related_soa<T>::value) {
                using SoA = typename T::related_soa;
                if (auto* arr = arch.get_array<SoA>())
                    arr->add(comps);
            }
            else {
                if (auto* arr = arch.get_array<T>())
                    arr->add(std::forward<T>(comps));
            }
        }()
            ));
}

template<typename T>
foreach_arg_t<T> ObjectManager::GetComponent(SceneData* scene, Entity e)
{
    auto arch_it = scene->entity_to_archetype.find(e);
    SDL_assert(arch_it != scene->entity_to_archetype.end());

    Archetype* arch = arch_it->second;
    auto* arr = arch->get_array<T>();
    SDL_assert(arr && "Component not found in archetype");

    auto idx_it = scene->entity_to_index.find(e);
    SDL_assert(idx_it != scene->entity_to_index.end());

    size_t idx = idx_it->second;
    SDL_assert(idx < arr->size());

    if constexpr (is_soa<T>::value) {
        return SoAElement<T>{ &arr->data, idx };
    }
    else {
        return (*arr)[idx];
    }
}

template<typename Component>
bool ObjectManager::Has(SceneData* scene, Entity e) const {
    auto arch_it = scene->entity_to_archetype.find(e);
    if (arch_it == scene->entity_to_archetype.end())
        return false;

    Archetype* arch = arch_it->second;
    return arch->get_array<Component>() != nullptr;
}
