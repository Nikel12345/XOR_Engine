#include "PCH.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <set>
#include <unordered_map>

SceneData* ObjectManager::CreateScene(const SceneName& name) {
    // Пустое имя — не сцена, а СЛЕД её отсутствия: так GetActiveSceneName сообщает «активной нет»,
    // и это значение доезжает сюда через UI-команды. Заведённая по нему сцена "" была бы активной
    // (is_active=true по умолчанию) и подменяла бы собой настоящую.
    if (name.empty()) {
        SDL_Log("CreateScene: empty scene name rejected (no active scene?)");
        return nullptr;
    }
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

    // КАСКАДА на детей здесь НЕТ — он в EngineContext::DeleteEntity, который снимает и рендер-
    // инстанс каждого ребёнка (QueueDelete); иначе их трансформ-строки остались бы в батче и
    // «переехали» бы на чужие объекты. Тут — бухгалтерия одного e.
    if (Has<ParentComponent>(scene, e)) {
        Entity parent = GetComponent<ParentComponent>(scene, e).parent;
        if (auto pit = scene->children.find(parent); pit != scene->children.end()) {
            auto& v = pit->second;
            v.erase(std::remove(v.begin(), v.end(), e), v.end());
            if (v.empty()) scene->children.erase(pit);
        }
    }
    // Штатно запись уже пуста — детей снял каскад. При ПРЯМОМ вызове на родителе они осиротеют:
    // отсюда и правило, что удаляют через EngineContext::DeleteEntity.
    scene->children.erase(e);

    Archetype* arch = arch_it->second;
    auto idx_it = scene->entity_to_index.find(e);
    SDL_assert(idx_it != scene->entity_to_index.end());
    const size_t i = idx_it->second;
    const size_t last = arch->entities.size() - 1;

    arch->swap_remove(i);

    if (i != last) {
        Entity moved = arch->entities[last];
        arch->entities[i] = moved;
        scene->entity_to_index[moved] = i;
    }
    arch->entities.pop_back();

    scene->entity_to_index.erase(e);
    scene->entity_to_archetype.erase(e);

    // Пересборку батчей НЕ взводим: удаление доезжает до дерева инкрементально, дельтой от
    // EngineContext::DeleteEntity (BatchBuilder::QueueDelete).
    ++entity_revision;
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

void ObjectManager::SetActiveScene(const SceneName& scene_name)
{
    auto it = scenes_data.find(scene_name);
    if (it == scenes_data.end()) {
        SDL_Log("SetActiveScene: scene '%s' not found", scene_name.c_str());
        return;
    }
    // Гасим ВСЕ и зажигаем одну: инвариант «активная ровно одна» держится тут, а не у вызывающих.
    for (auto& [name, scene] : scenes_data) scene->is_active = (name == scene_name);
    // Состав сущностей под обходами сменился целиком — гейтящиеся ревизией буферы перезальются.
    ++entity_revision;
}

SceneData* ObjectManager::GetActiveScene()
{
    for (auto& [name, scene] : scenes_data) {
        if (scene->is_active) {
            no_active_scene_reported = false;
            return scene.get();
        }
    }
    if (!no_active_scene_reported) {
        no_active_scene_reported = true;
        SDL_Log("No active scene found! (further reports suppressed until one becomes active)");
    }
    return nullptr;
}

SceneName ObjectManager::GetActiveSceneName()
{
    for (auto& [name, scene] : scenes_data) {
        if (scene->is_active) {
            no_active_scene_reported = false;
            return name;
        }
    }
    if (!no_active_scene_reported) {
        no_active_scene_reported = true;
        SDL_Log("No active scene found! (further reports suppressed until one becomes active)");
    }
    // ПУСТОЕ имя = «активной сцены нет», и потребители обязаны читать его так: CreateScene и
    // LoadScene его отвергают, UI на нём не рисует блок сцен и не шлёт команд.
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

// Пост-обработка pretty-вывода yyjson: массив БЕЗ объектов внутри схлопывается в одну строку.
// Своего флага у yyjson для этого нет, а колонка поля — ровно такой массив: в столбик scene.json
// раздувается до строки на КАЖДОЕ значение (архетип из N сущностей x M полей). Меняется только
// раскладка, не структура. Проход строкоосознанный: скобка внутри json-строки — литерал.
static std::string InlineScalarArrays(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (in_str) {
            out += c;
            if (c == '\\') { if (i + 1 < s.size()) out += s[++i]; }
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; out += c; continue; }
        if (c != '[')  { out += c; continue; }

        // Ищем парную ']', попутно отвергая объекты внутри и вложенность глубже ОДНОГО уровня.
        // Один уровень пускаем ради зубчатой колонки Material: иначе на каждую сущность уходит
        // отдельная строка "[0]," с полным отступом.
        size_t j = i + 1;
        int  depth = 1;
        bool bad = false, closed = false, str = false;
        for (; j < s.size(); ++j) {
            const char d = s[j];
            if (str) {
                if (d == '\\') ++j;
                else if (d == '"') str = false;
                continue;
            }
            if (d == '"') { str = true; continue; }
            if (d == '{') { bad = true; break; }       // объект — это структура, ей pretty нужен
            if (d == '[') { if (++depth > 2) { bad = true; break; } continue; }
            if (d == ']') { if (--depth == 0) { closed = true; break; } }
        }
        if (bad || !closed) { out += c; continue; }

        str = false;
        for (size_t k = i; k <= j; ++k) {
            const char d = s[k];
            if (str) {
                out += d;
                if (d == '\\') { if (k + 1 <= j) out += s[++k]; }
                else if (d == '"') str = false;
                continue;
            }
            if (d == '"') { str = true; out += d; continue; }
            if (d == ' ' || d == '\t' || d == '\n' || d == '\r') continue;
            out += d;
            if (d == ',') out += ' ';
        }
        i = j;
    }
    return out;
}

std::string ObjectManager::SaveScene(SceneData* scene)
{
    if (!scene) return {};
    auto& reg = ComponentSpecRegistry::Get();

    // Ключ верхнего объекта — сам архетип: имена его компонентов через запятую, отсортированные,
    // поэтому от порядка не зависит. Внутри — count, колонка entities и компоненты колонками.
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Блоки архетипов копим и кладём в корень ПОСЛЕ словаря: словарь заполняется по ходу их
    // записи, а в файле обязан стоять первым — иначе его не прочесть, не разобрав всю сцену.
    ScenePool pool;
    std::vector<std::pair<std::string, yyjson_mut_val*>> blocks;

    for (auto& [sig, arch] : scene->archetypes) {
        // Производные сущности в файл не идут — их пересоздаст генератор сцены.
        if (arch.components.count(std::type_index(typeid(GeneratedComponent)))) continue;
        const size_t count = arch.entities.size();
        if (count == 0) continue;

        std::vector<const ComponentSpec*> hs;
        for (auto& [tindex, arr] : arch.components)
            if (const ComponentSpec* h = reg.ByType(tindex)) hs.push_back(h);
        if (hs.empty()) continue;

        // Сортировка нужна не только ключу: в ЭТОМ же порядке пишутся блоки компонентов, а
        // arch.components — unordered_map, и без сортировки пересохранение той же сцены тасовало
        // бы файл.
        std::sort(hs.begin(), hs.end(),
                  [](const ComponentSpec* a, const ComponentSpec* b) { return a->name < b->name; });
        std::string key;
        for (size_t i = 0; i < hs.size(); ++i) { if (i) key += ','; key += hs[i]->name; }

        yyjson_mut_val* block = yyjson_mut_obj(doc);
        blocks.emplace_back(key, block);
        yyjson_mut_obj_add_uint(doc, block, "count", count);

        // entities пишем всегда: по этим файл-локальным id загрузка ремапит Parent.
        yyjson_mut_val* ents = yyjson_mut_obj_add_arr(doc, block, "entities");
        for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_uint(doc, ents, arch.entities[i]);

        for (const ComponentSpec* h : hs) {
            yyjson_mut_val* comp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add(block, yyjson_mut_strcpy(doc, h->name.c_str()), comp);
            h->Save(arch, count, doc, comp, &pool);
        }
    }

    pool.Write(doc, root);

    // Порядок обхода scene->archetypes — порядок std::type_index в ключе, стандартом он не
    // определён. Сортировка по ключу-архетипу делает файл детерминированным, а вместе с ним и
    // файл-локальные id: их раздаёт загрузка ПО ПОРЯДКУ блоков.
    std::sort(blocks.begin(), blocks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [key, block] : blocks)
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(doc, key.c_str()), block);   // ключ динамический → strcpy

    // FP_TO_FLOAT: числовые поля схемы объявлены F32, а double в аксессорах — лишь общий канал
    // доступа к ним. Двойная точность на письме — это ~7 лишних байт на КАЖДОЕ число (на 1M
    // сущностей треть файла), и на чтении они всё равно сужаются обратно. Потери нет.
    char* js = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_FP_TO_FLOAT, nullptr);
    std::string out = js ? InlineScalarArrays(js) : std::string{};
    if (js) free(js);
    yyjson_mut_doc_free(doc);
    return out;
}

std::vector<Entity> ObjectManager::LoadScene(const SceneName& scene_name, const std::string& text)
{
    std::vector<Entity> created;

    auto sit = scenes_data.find(scene_name);
    // Автосоздание по имени — штатный путь первой загрузки, отдельный CreateScene игре не нужен.
    // Пустое имя при этом отвергает сам CreateScene (см. там) — отсюда проверка на null ниже.
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    if (!scene) {
        SDL_Log("ObjectManager::LoadScene: no scene for name '%s' - nothing loaded", scene_name.c_str());
        return created;
    }
    auto& reg = ComponentSpecRegistry::Get();

    std::unordered_map<uint32_t, Entity> old_to_new;     // файл-локальный id → новый Entity

    const auto t_pass1 = Prof::Clock::now();
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) { SDL_Log("LoadScene: scene.json parse failed"); return created; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) { SDL_Log("LoadScene: scene.json root is not object"); yyjson_doc_free(doc); return created; }

    // Словарь имён — ДО архетипов: их колонки ассетов ссылаются в него индексами.
    ScenePool pool;
    pool.Read(root);

    size_t est = 0;
    {
        size_t ak, am; yyjson_val *an, *ab;
        yyjson_obj_foreach(root, ak, am, an, ab) est += (size_t)yyjson_get_uint(yyjson_obj_get(ab, "count"));
    }
    created.reserve(est);
    old_to_new.reserve(est * 2);
    scene->entity_to_archetype.reserve(scene->entity_to_archetype.size() + est);
    scene->entity_to_index.reserve(scene->entity_to_index.size() + est);

    // Проход 1: на каждый архетип — создать count сущностей и залить колонки компонентов.
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

            // Сущности создаём ДО заливки колонок: ComponentSpec::Load дописывает строки в хвост
            // и считает базу от entities.size() — порядок add обязан совпасть с arch.entities.
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
                h->Load(arch, comp, count, &pool);
            }
        }
    }
    const double pass1_ms = Prof::MsSince(t_pass1);

    // Проход 2: в ParentComponent лежит файл-локальный id — меняем его на настоящий Entity и
    // заполняем scene->children. Отдельным проходом, потому что родитель мог приехать позже
    // ребёнка; после прохода 1 все сущности уже есть, так что ни глубина, ни циклы не важны.
    const auto t_pass2 = Prof::Clock::now();
    for (Entity e : created) {
        if (!Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = GetComponent<ParentComponent>(scene, e);
        auto it = old_to_new.find(pc.parent);
        if (it == old_to_new.end()) {
            // Родитель не сохранён (был производным). Самоссылка — существующий id, без падения.
            SDL_Log("LoadScene: parent id %u unresolved for entity %u - hierarchy dropped", pc.parent, e);
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
    // Индексы, которым нет имени в шапке: словарь правили мимо колонок. Счётчик, а не лог на
    // месте — иначе на миллионе сущностей это миллион строк.
    if (pool.Misses())
        SDL_Log("LoadScene: %u asset cells reference a missing dictionary entry - names dropped", pool.Misses());

    ++entity_revision;
    return created;
}

Entity ObjectManager::CreateEntityFromSpecs(SceneData* scene, const std::vector<const ComponentSpec*>& specs)
{
    if (!scene || specs.empty()) { SDL_Log("CreateEntityFromSpecs: null scene or empty set"); return static_cast<Entity>(-1); }

    std::set<std::type_index> sig;
    for (const ComponentSpec* s : specs) sig.insert(s->sig_type);

    Archetype& arch = scene->archetypes[sig];
    Entity e = scene->next_entity_id++;
    // Сущность — ДО Load'ов, тот же инвариант, что в LoadScene.
    arch.entities.push_back(e);
    scene->entity_to_archetype[e] = &arch;
    scene->entity_to_index[e] = arch.entities.size() - 1;

    for (const ComponentSpec* s : specs) s->Load(arch, nullptr, 1, nullptr);   // comp==nullptr = дефолтный ряд

    return e;
}

