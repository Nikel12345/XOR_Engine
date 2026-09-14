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

    // Сигнатура архетипа и его колонки — по типу ХРАНИЛИЩА: прокси и его SoA (PositionProxy16 и
    // Positions) — один компонент, и архетип обязан знать его под одним именем.
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

    // parent снимаем ДО add_components: там comps форвардятся в хранилище, и читать их уже поздно.
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

    if (has_parent)
        scene->children[parent_id].push_back(e);

	++entity_revision;

    return e;
}

template<typename... Ts, typename Fn>
void ObjectManager::ForEachArchetype(SceneData* scene, Fn&& fn) {
    if (!scene) return;

    // Вторая форма (колонки + entities архетипа): горячему проходу нередко нужен id сущности —
    // адресовать её командой, сменить материал, — а отдать его, не уходя в поэлементный обход,
    // больше негде. Подробнее о формах — в ObjectManager.h.
    constexpr bool wants_entity_list =
        std::is_invocable_v<std::decay_t<Fn>, ComponentArray<Ts>*..., const std::vector<Entity>&>;

    for (auto& [sig, arch] : scene->archetypes) {
        auto arrs = std::tuple{ arch.get_array<Ts>()... };
        bool all_present = std::apply([](auto*... arr) {
            return (... && (arr != nullptr));
            }, arrs);
        if (!all_present) continue;

        if constexpr (wants_entity_list) {
            std::apply([&](auto*... arr) {
                fn(arr..., arch.entities);
                }, arrs);
        }
        else {
            std::apply([&](auto*... arr) {
                fn(arr...);
                }, arrs);
        }
    }
}



template<typename T>
std::enable_if_t<!is_soa<T>::value, T&>
make_param(ComponentArray<T>* arr, size_t i) {
    return (*arr)[i];
}

template<typename T>
std::enable_if_t<is_soa<T>::value, SoAElement<T>>
make_param(ComponentArray<T>* arr, size_t i) {
    return SoAElement<T>{ &arr->data, i };
}


template<typename... Ts, typename Fn>
void ObjectManager::ForEach(SceneData* scene, Fn&& fn) {
    // Нет сцены — пустой обход, и МОЛЧА: путь покадровый, лог превратился бы в сотни одинаковых
    // строк в секунду. Само состояние называет GetActiveScene, один раз на вход в него.
    if (!scene) return;

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
    // Тихо вернуть «ничего», как Has, здесь нельзя — возвращается ССЫЛКА. Поэтому нулевая сцена
    // ловится assert'ом: отсеять её обязан вызывающий, штатно — тем же Has.
    SDL_assert(scene && "GetComponent on null scene - gate it with Has() first");
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
    // Нет сцены — нет и компонента. Это ГЛАВНЫЙ фильтр отсутствующей сцены: почти каждый доступ к
    // компоненту стоит за Has, поэтому проверка здесь снимает её разом на всех этих путях. Тихо,
    // а не assert: «сцены нет» — легальное состояние движка (пустой кадр), а не ошибка вызывающего.
    if (!scene) return false;
    auto arch_it = scene->entity_to_archetype.find(e);
    if (arch_it == scene->entity_to_archetype.end())
        return false;

    Archetype* arch = arch_it->second;
    return arch->get_array<Component>() != nullptr;
}
