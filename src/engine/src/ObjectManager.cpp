#include "PCH.h"
#include "ObjectManager.h"
#include "TextureData.h"
#include "ModelData.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"   // Prof::Clock/MsSince — тайминг фаз парса (без GPU-зависимостей, безопасно для ECS-ядра)
#include <algorithm>   // std::remove (отцепление ребёнка), std::sort/std::unique (сигнатура)
#include <charconv>    // std::from_chars — быстрый парс id сущности/родителя без locale
#include <cstring>     // memchr/memcmp — посимвольный сканер текста сцены
#include <string_view> // токены-срезы в исходный буфер (без копий)
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
    //std::lock_guard<std::mutex> lock(ecs_mutex_);   // ДИАГНОСТИКА гонки (см. EcsMutex) — ВРЕМЕННО СНЯТО
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
    std::string out;
    if (!scene) return out;
    auto& reg = ComponentSerializerRegistry::Get();

    for (auto& [sig, arch] : scene->archetypes) {
        // Сгенерированные кодом сущности (debug-рамки и т.п.) в файл не идут — их пересоздаст
        // генератор при загрузке. Маркер — GeneratedComponent (НЕ EditorHidden: тот лишь
        // прячет из списка UI и к персистентности отношения не имеет).
        if (arch.components.count(std::type_index(typeid(GeneratedComponent)))) continue;

        for (size_t i = 0; i < arch.entities.size(); ++i) {
            std::string body;
            for (auto& [tindex, arr] : arch.components) {
                const ComponentSerializer* h = reg.ByType(tindex);
                if (!h) continue;                       // тип без сериалайзера — пропускаем
                std::string payload;
                h->save(arch, i, payload);
                body += "  ";
                body += h->name;
                body += " =";
                if (!payload.empty()) { body += ' '; body += payload; }
                body += '\n';
            }
            if (!body.empty()) {                        // нечего сохранять — нет и блока
                // id сущности в заголовке — стабильный ключ для ссылок Parent (ремап при загрузке).
                out += "[entity] ";
                out += std::to_string(arch.entities[i]);
                out += '\n';
                out += body;
            }
        }
    }
    return out;
}

std::vector<Entity> ObjectManager::LoadScene(const SceneName& scene_name, const std::string& text)
{
    //std::lock_guard<std::mutex> lock(ecs_mutex_);   // ДИАГНОСТИКА гонки (см. EcsMutex) — ВРЕМЕННО СНЯТО
    auto sit = scenes_data.find(scene_name);
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    auto& reg = ComponentSerializerRegistry::Get();

    // Пул компонентов текущей сущности. Переиспользуется между сущностями: n_comps
    // сбрасывается в 0 (а не clear()), поэтому внутренние векторы токенов сохраняют
    // ёмкость — на группе одинаковых архетипов аллокаций почти нет. Токены — это
    // string_view В ИСХОДНЫЙ text (жив всю функцию), без копий строк.
    struct Pending { const ComponentSerializer* h = nullptr; std::vector<std::string_view> tokens; };
    std::vector<Pending> comps;
    size_t   n_comps = 0;
    uint32_t cur_id = 0;
    bool     have_block = false;
    uint32_t synthetic = 0xFFFF0000u;                    // id для блоков без явного (back-compat)

    // Кэш «последняя сигнатура → архетип». В файле сущности идут ГРУППАМИ своего
    // архетипа, поэтому подряд идущие дают тот же набор типов. Пока сигнатура не
    // сменилась — переиспользуем указатель, минуя пересборку std::set и поиск в
    // std::map (это и был главный пожиратель pass1). Указатель на Archetype стабилен:
    // узлы std::map не переезжают при вставке новых архетипов (как и в остальном ECS).
    std::vector<std::type_index> last_sig;               // отсортирован; сравнивается с текущим
    std::vector<std::type_index> cur_sig;
    Archetype* last_arch = nullptr;

    std::unordered_map<uint32_t, Entity> old_to_new;     // id из файла → новый Entity
    std::vector<Entity> created;

    // Предпроход: считаем заголовки (строки, начинающиеся с '[') — точная оценка числа
    // сущностей. Резервируем карты/векторы, чтобы 200k вставок не давали ре-хэшей.
    // Один линейный проход по 43 МБ ≈ единицы мс на фоне парса.
    size_t est_entities = 0;
    {
        const char* c = text.data();
        const char* e = c + text.size();
        bool bol = true;
        for (; c < e; ++c) {
            if (bol && *c == '[') ++est_entities;
            bol = (*c == '\n');
        }
    }
    created.reserve(est_entities);
    old_to_new.reserve(est_entities * 2);
    scene->entity_to_archetype.reserve(scene->entity_to_archetype.size() + est_entities);
    scene->entity_to_index.reserve(scene->entity_to_index.size() + est_entities);

    auto flush = [&]() {
        if (!have_block) { n_comps = 0; return; }
        have_block = false;
        if (n_comps == 0) return;

        // Сигнатура = отсортированный уникальный набор sig_type компонентов.
        cur_sig.clear();
        for (size_t i = 0; i < n_comps; ++i) cur_sig.push_back(comps[i].h->sig_type);
        std::sort(cur_sig.begin(), cur_sig.end());
        cur_sig.erase(std::unique(cur_sig.begin(), cur_sig.end()), cur_sig.end());

        Archetype* arch;
        if (last_arch && cur_sig == last_sig) {
            arch = last_arch;                              // та же группа — без set/map
        }
        else {
            std::set<std::type_index> sig(cur_sig.begin(), cur_sig.end());
            arch = &scene->archetypes[sig];
            last_sig = cur_sig;
            last_arch = arch;
        }

        Entity e = scene->next_entity_id++;
        arch->entities.push_back(e);
        for (size_t i = 0; i < n_comps; ++i)
            comps[i].h->load(*arch, comps[i].tokens);      // ensure_component<T> + дописать
        scene->entity_to_archetype[e] = arch;
        scene->entity_to_index[e] = arch->entities.size() - 1;

        old_to_new[cur_id] = e;
        created.push_back(e);
        n_comps = 0;
    };

    // Взять/переиспользовать слот компонента (векторы токенов сохраняют ёмкость).
    auto push_comp = [&](const ComponentSerializer* h) -> std::vector<std::string_view>& {
        if (n_comps == comps.size()) comps.emplace_back();
        Pending& pc = comps[n_comps++];
        pc.h = h;
        pc.tokens.clear();
        return pc.tokens;
    };

    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };

    const auto t_pass1 = Prof::Clock::now();   // фаза 1: парс текста + сборка архетипов

    // Однопроходный посимвольный сканер по всему буферу — без istringstream/getline,
    // без substr и без промежуточных std::string. Строки ограничиваем memchr('\n').
    const char* p   = text.data();
    const char* end = p + text.size();
    while (p < end) {
        const char* nl       = static_cast<const char*>(std::memchr(p, '\n', end - p));
        const char* line_end = nl ? nl : end;
        const char* next     = nl ? nl + 1 : end;

        // Trim строки → [a, b).
        const char* a = p;
        const char* b = line_end;
        while (a < b && is_space(*a)) ++a;
        while (b > a && is_space(b[-1])) --b;
        p = next;

        if (a == b) continue;                 // пустая
        if (*a == '#') continue;              // комментарий

        // Заголовок сущности: "[entity] <id>"
        if (b - a >= 8 && std::memcmp(a, "[entity]", 8) == 0) {
            flush();
            const char* q = a + 8;
            while (q < b && is_space(*q)) ++q;
            if (q < b) {
                uint32_t id = 0;
                std::from_chars(q, b, id);
                cur_id = id;
            }
            else {
                cur_id = synthetic++;
            }
            have_block = true;
            continue;
        }

        if (!have_block) continue;

        // Строка компонента: "<Name> = <tokens...>"
        const char* eq       = static_cast<const char*>(std::memchr(a, '=', b - a));
        const char* name_end = eq ? eq : b;
        while (name_end > a && is_space(name_end[-1])) --name_end;   // trim имени справа

        const ComponentSerializer* h = reg.ByName(std::string(a, name_end));  // имя короткое → SSO
        if (!h) continue;                                                     // незнакомый компонент

        std::vector<std::string_view>& toks = push_comp(h);
        if (eq) {
            const char* q = eq + 1;
            while (q < b) {
                while (q < b && is_space(*q)) ++q;
                if (q >= b) break;
                const char* ts = q;
                while (q < b && !is_space(*q)) ++q;
                toks.emplace_back(ts, static_cast<size_t>(q - ts));
            }
        }
    }
    flush();
    const double pass1_ms = Prof::MsSince(t_pass1);

    // Проход 2: ParentComponent сейчас держит СТАРЫЙ id из файла. Ремапим в новый Entity
    // и заполняем обратный индекс scene->children (CreateEntity делает это при создании —
    // здесь путь иной, поэтому вручную). Родитель уже создан: все сущности есть после прохода 1.
    const auto t_pass2 = Prof::Clock::now();   // фаза 2: ремап родителей + обратный индекс
    for (Entity e : created) {
        if (!Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = GetComponent<ParentComponent>(scene, e);
        auto it = old_to_new.find(pc.parent);
        if (it == old_to_new.end()) {
            // Родитель не сохранён (напр. был скрыт EditorHidden). Висячий id уронил бы
            // трансформ-модуль — делаем самоссылку (существует, без падения) и логируем.
            SDL_Log("LoadScene: parent id %u unresolved for entity %u — hierarchy dropped", pc.parent, e);
            pc.parent = e;
            continue;
        }
        pc.parent = it->second;
        scene->children[pc.parent].push_back(e);
    }
    const double pass2_ms = Prof::MsSince(t_pass2);

    SDL_Log("  ObjectManager::LoadScene: pass1(parse+build)=%.1f  pass2(parent remap)=%.1f ms  [%zu ent, %zu archetypes]",
        pass1_ms, pass2_ms, created.size(), scene->archetypes.size());

    dirty_entity = true;
    return created;
}

