#include "PCH.h"
#include "ObjectManager.h"
#include "TextureData.h"
#include "ModelData.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"   // Prof::Clock/MsSince — тайминг фаз парса (без GPU-зависимостей, безопасно для ECS-ядра)
#include <algorithm>   // std::remove (отцепление ребёнка), std::sort (ключ-сигнатура архетипа)
#include <cstring>     // strcmp — отсев служебных ключей count/entities
#include <cstdlib>     // free — строка от yyjson_mut_write
#include <set>         // std::set<type_index> — ключ архетипа в scene->archetypes
#include <unordered_map>   // old_to_new (файл-локальный id → Entity)
// RenderManager.h/PipeManager.h не использовались — убраны, чтобы ECS-ядро
// (EngineEcs) не тянуло GPU-заголовки.

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

    // Поддержка обратного индекса иерархии. КАСКАД на детей здесь НЕ делаем — он в
    // EngineContext::DeleteEntity, который умеет снять и рендер-инстанс ребёнка
    // (QueueDelete); иначе трансформ-строка рамки осталась бы в батче и «переехала»
    // бы на чужой объект. Тут — только бухгалтерия одного e.
    // Отцепить e из списка детей его родителя (если e сам — чей-то ребёнок).
    if (Has<ParentComponent>(scene, e)) {
        Entity parent = GetComponent<ParentComponent>(scene, e).parent;
        if (auto pit = scene->children.find(parent); pit != scene->children.end()) {
            auto& v = pit->second;
            v.erase(std::remove(v.begin(), v.end(), e), v.end());
            if (v.empty()) scene->children.erase(pit);
        }
    }
    // Снять собственную запись детей. Штатно к этому моменту она уже пуста (детей
    // удалил каскад EngineContext); при ПРЯМОМ вызове этого метода на родителе дети
    // осиротеют — поэтому штатный путь удаления только через EngineContext::DeleteEntity.
    scene->children.erase(e);

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

// ============================================================
//  Сериализация сцены
// ============================================================
std::string ObjectManager::SaveScene(SceneData* scene)
{
    if (!scene) return {};
    auto& reg = ComponentSpecRegistry::Get();

    // Верх — объект; ключ = сам архетип (отсортированные имена компонентов через запятую,
    // порядконезависимо). Внутри: count + колонка entities (файл-локальные id) + компоненты-колонки.
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    for (auto& [sig, arch] : scene->archetypes) {
        // Сгенерированные кодом (debug-рамки и т.п.) в файл не идут — пересоздаст генератор.
        if (arch.components.count(std::type_index(typeid(GeneratedComponent)))) continue;
        const size_t count = arch.entities.size();
        if (count == 0) continue;

        std::vector<const ComponentSpec*> hs;
        std::vector<std::string> names;
        for (auto& [tindex, arr] : arch.components) {
            const ComponentSpec* h = reg.ByType(tindex);
            if (h) { hs.push_back(h); names.push_back(h->name); }
        }
        if (hs.empty()) continue;

        std::sort(names.begin(), names.end());   // порядконезависимый ключ архетипа
        std::string key;
        for (size_t i = 0; i < names.size(); ++i) { if (i) key += ','; key += names[i]; }

        yyjson_mut_val* block = yyjson_mut_obj(doc);
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(doc, key.c_str()), block);   // ключ динамический → strcpy
        yyjson_mut_obj_add_uint(doc, block, "count", count);

        // entities — файл-локальные id, всегда (нужны для ремапа Parent на загрузке).
        yyjson_mut_val* ents = yyjson_mut_obj_add_arr(doc, block, "entities");
        for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_uint(doc, ents, arch.entities[i]);

        for (const ComponentSpec* h : hs) {
            yyjson_mut_val* comp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add(block, yyjson_mut_strcpy(doc, h->name.c_str()), comp);
            h->Save(arch, count, doc, comp);   // колонки по полям (генератор схемы или custom)
        }
    }

    char* js = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string out = js ? std::string(js) : std::string{};
    if (js) free(js);
    yyjson_mut_doc_free(doc);
    return out;
}

std::vector<Entity> ObjectManager::LoadScene(const SceneName& scene_name, const std::string& text)
{
    auto sit = scenes_data.find(scene_name);
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    auto& reg = ComponentSpecRegistry::Get();

    std::vector<Entity> created;
    std::unordered_map<uint32_t, Entity> old_to_new;     // файл-локальный id → новый Entity

    const auto t_pass1 = Prof::Clock::now();   // фаза 1: парс json + сборка архетипов
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) { SDL_Log("LoadScene: scene.json parse failed"); return created; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) { SDL_Log("LoadScene: scene.json root is not object"); yyjson_doc_free(doc); return created; }

    // Оценка числа сущностей (сумма count по архетипам) — reserve, чтобы вставки не ре-хэшили.
    size_t est = 0;
    {
        size_t ak, am; yyjson_val *an, *ab;
        yyjson_obj_foreach(root, ak, am, an, ab) est += (size_t)yyjson_get_uint(yyjson_obj_get(ab, "count"));
    }
    created.reserve(est);
    old_to_new.reserve(est * 2);
    scene->entity_to_archetype.reserve(scene->entity_to_archetype.size() + est);
    scene->entity_to_index.reserve(scene->entity_to_index.size() + est);

    // Проход 1: по архетипам — создать count сущностей, залить колонки компонентов.
    {
        size_t ak, am; yyjson_val *aname, *block;
        yyjson_obj_foreach(root, ak, am, aname, block) {
            if (!yyjson_is_obj(block)) continue;

            // Компоненты архетипа = ключи блока, кроме служебных count/entities.
            std::vector<const ComponentSpec*> hs;
            std::set<std::type_index> sig;
            {
                size_t ck, cm; yyjson_val *cname, *cval;
                yyjson_obj_foreach(block, ck, cm, cname, cval) {
                    const char* nm = yyjson_get_str(cname);
                    if (!nm || !std::strcmp(nm, "count") || !std::strcmp(nm, "entities")) continue;
                    const ComponentSpec* h = reg.ByName(nm);
                    if (h) { hs.push_back(h); sig.insert(h->sig_type); }
                }
            }
            yyjson_val* ents  = yyjson_obj_get(block, "entities");
            yyjson_val* cnt_v = yyjson_obj_get(block, "count");
            size_t count = cnt_v ? (size_t)yyjson_get_uint(cnt_v) : (ents ? yyjson_arr_size(ents) : 0);
            if (count == 0 || hs.empty()) continue;

            Archetype& arch = scene->archetypes[sig];

            // Файл-локальные id (для ремапа Parent). Создаём count сущностей ДО заливки колонок:
            // индексы в arch.entities совпадут с порядком add в load каждого компонента.
            std::vector<uint32_t> ids(count, 0);
            if (ents) { size_t i, m; yyjson_val* v; yyjson_arr_foreach(ents, i, m, v) { if (i >= count) break; ids[i] = (uint32_t)yyjson_get_uint(v); } }
            for (size_t i = 0; i < count; ++i) {
                Entity e = scene->next_entity_id++;
                arch.entities.push_back(e);
                scene->entity_to_archetype[e] = &arch;
                scene->entity_to_index[e] = arch.entities.size() - 1;
                old_to_new[ids[i]] = e;
                created.push_back(e);
            }

            for (const ComponentSpec* h : hs) {
                yyjson_val* comp = yyjson_obj_get(block, h->name.c_str());
                h->Load(arch, comp, count);   // ensure_component<T> + ровно count add
            }
        }
    }
    const double pass1_ms = Prof::MsSince(t_pass1);

    // Проход 2: ParentComponent держит файл-локальный old id. Ремапим в новый Entity и заполняем
    // обратный индекс scene->children. Все сущности уже есть после прохода 1 — глубина/циклы не важны.
    const auto t_pass2 = Prof::Clock::now();
    for (Entity e : created) {
        if (!Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = GetComponent<ParentComponent>(scene, e);
        auto it = old_to_new.find(pc.parent);
        if (it == old_to_new.end()) {
            // Родитель не сохранён (напр. был скрыт/Generated). Самоссылка — существует, без падения.
            SDL_Log("LoadScene: parent id %u unresolved for entity %u — hierarchy dropped", pc.parent, e);
            pc.parent = e;
            continue;
        }
        pc.parent = it->second;
        scene->children[pc.parent].push_back(e);
    }
    const double pass2_ms = Prof::MsSince(t_pass2);

    yyjson_doc_free(doc);

    SDL_Log("  ObjectManager::LoadScene: pass1(parse+build)=%.1f  pass2(parent remap)=%.1f ms  [%zu ent, %zu archetypes]",
        pass1_ms, pass2_ms, created.size(), scene->archetypes.size());

    dirty_entity = true;
    return created;
}

Entity ObjectManager::CreateEntityFromSpecs(SceneData* scene, const std::vector<const ComponentSpec*>& specs)
{
    if (!scene || specs.empty()) { SDL_Log("CreateEntityFromSpecs: null scene or empty set"); return static_cast<Entity>(-1); }

    std::set<std::type_index> sig;
    for (const ComponentSpec* s : specs) sig.insert(s->sig_type);

    Archetype& arch = scene->archetypes[sig];
    Entity e = scene->next_entity_id++;
    // Сущность — ДО Load'ов: инвариант ComponentSpec::Load (строки дописываются в хвост,
    // base считается от entities.size()), тот же порядок, что в LoadScene.
    arch.entities.push_back(e);
    scene->entity_to_archetype[e] = &arch;
    scene->entity_to_index[e] = arch.entities.size() - 1;

    for (const ComponentSpec* s : specs) s->Load(arch, nullptr, 1);   // дефолтный ряд каждого

    return e;
}

