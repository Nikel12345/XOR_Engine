#include "PCH.h"
#include "ObjectManager.h"
#include "BaseComponents.h"   // ParentComponent/GeneratedComponent — иерархия и отбор при сохранении
#include "TextureData.h"
#include "ModelData.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <set>
#include <unordered_map>
// RenderManager.h/PipeManager.h не использовались — убраны, чтобы ECS-ядро
// (EngineEcs) не тянуло GPU-заголовки.

SceneData* ObjectManager::CreateScene(const SceneName& name) {
    // Пустое имя — не сцена, а СЛЕД отсутствующей активной сцены: GetActiveSceneName возвращает {},
    // и это значение утекает в UI-команды (SceneIOCmd/CreateEntityCmd). Раньше оно доезжало сюда и
    // заводило живую сцену "" — активную по умолчанию, то есть подменяющую собой настоящую.
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

void ObjectManager::SetActiveScene(const SceneName& scene_name)
{
    auto it = scenes_data.find(scene_name);
    if (it == scenes_data.end()) {
        SDL_Log("SetActiveScene: scene '%s' not found", scene_name.c_str());
        return;
    }
    // Гасим ВСЕ и зажигаем одну: инвариант «активная ровно одна» держится тут, а не у вызывающих
    // (см. ObjectManager.h). Сцены наперечёт - линейный проход бесплатен.
    for (auto& [name, scene] : scenes_data) scene->is_active = (name == scene_name);
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
    return nullptr; // не найдено
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
    // ПУСТОЕ имя — не сцена. Потребители обязаны считать его «нет активной сцены»: CreateScene и
    // ObjectManager::LoadScene его отвергают, UI на нём не рисует блок сцен и не шлёт команд.
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

// Пост-обработка pretty-вывода yyjson: массив БЕЗ объектов внутри схлопывается в одну строку
// ("x": [1.0, 2.0, 3.0] и зубчатое "names": [[0], [3], [0]]). Своего флага у yyjson нет (см. YYJSON_WRITE_*),
// а колонка поля — ровно такой массив: в столбик scene.json раздувается до строки на ЗНАЧЕНИЕ
// (архетип из N сущностей × M полей). Структура файла при этом не меняется — только раскладка.
// Проход строкоосознанный: '[' и ']' внутри json-строки не считаются скобками (перевод строки
// внутри строки yyjson всегда экранирует, а вот скобка там — обычный литерал).
static std::string InlineScalarArrays(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (in_str) {
            out += c;
            if (c == '\\') { if (i + 1 < s.size()) out += s[++i]; }   // экранированная пара — целиком
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; out += c; continue; }
        if (c != '[')  { out += c; continue; }

        // Парная ']' + заодно проверка, что внутри нет объектов и вложенность не глубже
        // ОДНОГО уровня массивов. Один уровень пускаем ради зубчатой колонки Material: иначе на
        // КАЖДУЮ сущность уходит отдельная строка "[0]," с полным отступом.
        size_t j = i + 1;
        int  depth = 1;                                // глубина относительно текущей '['
        bool bad = false, closed = false, str = false;
        for (; j < s.size(); ++j) {
            const char d = s[j];
            if (str) {
                if (d == '\\') ++j;
                else if (d == '"') str = false;
                continue;
            }
            if (d == '"') { str = true; continue; }
            if (d == '{') { bad = true; break; }       // объект — структура, ему нужен pretty
            if (d == '[') { if (++depth > 2) { bad = true; break; } continue; }
            if (d == ']') { if (--depth == 0) { closed = true; break; } }
        }
        if (bad || !closed) { out += c; continue; }

        str = false;                                   // плоский массив → без переносов и отступов
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

    // Верх — объект; ключ = сам архетип (отсортированные имена компонентов через запятую,
    // порядконезависимо). Внутри: count + колонка entities (файл-локальные id) + компоненты-колонки.
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Блоки архетипов копим и добавляем в корень ПОСЛЕ словаря: заполняется он по ходу их
    // записи, а в файле обязан стоять первым — иначе его не прочесть, не разобрав всю сцену.
    ScenePool pool;
    std::vector<std::pair<std::string, yyjson_mut_val*>> blocks;   // ключ-архетип → блок

    for (auto& [sig, arch] : scene->archetypes) {
        // Сгенерированные кодом (debug-рамки и т.п.) в файл не идут — пересоздаст генератор.
        if (arch.components.count(std::type_index(typeid(GeneratedComponent)))) continue;
        const size_t count = arch.entities.size();
        if (count == 0) continue;

        std::vector<const ComponentSpec*> hs;
        for (auto& [tindex, arr] : arch.components)
            if (const ComponentSpec* h = reg.ByType(tindex)) hs.push_back(h);
        if (hs.empty()) continue;

        // Сортировка по имени — не только ради порядконезависимого ключа архетипа: в ЭТОМ же
        // порядке ниже пишутся блоки компонентов. Обход arch.components — обход unordered_map,
        // его порядок разъезжается между сценами с ОДИНАКОВЫМ составом, и без сортировки
        // пересохранение той же сцены тасовало бы блоки в файле (диффы на ровном месте).
        std::sort(hs.begin(), hs.end(),
                  [](const ComponentSpec* a, const ComponentSpec* b) { return a->name < b->name; });
        std::string key;
        for (size_t i = 0; i < hs.size(); ++i) { if (i) key += ','; key += hs[i]->name; }

        yyjson_mut_val* block = yyjson_mut_obj(doc);
        blocks.emplace_back(key, block);
        yyjson_mut_obj_add_uint(doc, block, "count", count);

        // entities — файл-локальные id, всегда (нужны для ремапа Parent на загрузке).
        yyjson_mut_val* ents = yyjson_mut_obj_add_arr(doc, block, "entities");
        for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_uint(doc, ents, arch.entities[i]);

        for (const ComponentSpec* h : hs) {
            yyjson_mut_val* comp = yyjson_mut_obj(doc);
            yyjson_mut_obj_add(block, yyjson_mut_strcpy(doc, h->name.c_str()), comp);
            h->Save(arch, count, doc, comp, &pool);   // колонки по полям (генератор схемы или custom)
        }
    }

    pool.Write(doc, root);

    // Блоки — по ключу архетипа. Обход scene->archetypes идёт по unordered_map, и у двух сцен
    // с одинаковым составом его порядок разный; без сортировки пересохранение переставляет
    // блоки, а вместе с ними и файл-локальные id — их раздаёт загрузка ПО ПОРЯДКУ блоков.
    std::sort(blocks.begin(), blocks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [key, block] : blocks)
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(doc, key.c_str()), block);   // ключ динамический → strcpy

    // FP_TO_FLOAT: числовые поля схемы объявлены F32, а double — лишь общий канал доступа к ним.
    // Двойная точность на письме — это ~7 лишних байт на КАЖДОЕ число (на 1M сущностей треть
    // файла), причём на чтении они всё равно сужаются обратно до float. Потери здесь нет.
    char* js = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_FP_TO_FLOAT, nullptr);
    std::string out = js ? InlineScalarArrays(js) : std::string{};   // колонки — в строчку
    if (js) free(js);
    yyjson_mut_doc_free(doc);
    return out;
}

std::vector<Entity> ObjectManager::LoadScene(const SceneName& scene_name, const std::string& text)
{
    std::vector<Entity> created;

    auto sit = scenes_data.find(scene_name);
    // Автосоздание по имени — штатный путь первой загрузки (игре не нужен отдельный CreateScene),
    // но ТОЛЬКО для настоящего имени: пустое приезжает из UI при отсутствующей активной сцене и
    // завело бы сцену "" (CreateScene её теперь отвергает, отсюда и повторная проверка на null).
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    if (!scene) {
        SDL_Log("ObjectManager::LoadScene: no scene for name '%s' - nothing loaded", scene_name.c_str());
        return created;
    }
    auto& reg = ComponentSpecRegistry::Get();

    std::unordered_map<uint32_t, Entity> old_to_new;     // файл-локальный id → новый Entity

    const auto t_pass1 = Prof::Clock::now();   // фаза 1: парс json + сборка архетипов
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) { SDL_Log("LoadScene: scene.json parse failed"); return created; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) { SDL_Log("LoadScene: scene.json root is not object"); yyjson_doc_free(doc); return created; }

    // Словарь имён — ДО архетипов: их колонки ассетов ссылаются в него индексами.
    ScenePool pool;
    pool.Read(root);

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
                h->Load(arch, comp, count, &pool);   // ensure_component<T> + ровно count add
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
    // Индекс, которому нет имени в шапке: файл рассогласован (правили словарь мимо колонок).
    // Копили счётчик, а не логировали на месте — иначе на миллионе сущностей это миллион строк.
    if (pool.Misses())
        SDL_Log("LoadScene: %u asset cells reference a missing dictionary entry - names dropped", pool.Misses());

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

    for (const ComponentSpec* s : specs) s->Load(arch, nullptr, 1, nullptr);   // дефолтный ряд каждого

    return e;
}

