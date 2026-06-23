#include "PCH.h"
#include "ObjectManager.h"
#include "TextureData.h"
#include "ModelData.h"
#include "ComponentSerializer.h"
#include <algorithm>   // std::remove для отцепления ребёнка из обратного индекса
#include <sstream>     // разбор текста сцены построчно
#include <cstdlib>     // strtoul — разбор id сущности/родителя
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
namespace {
    void Trim(std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) { s.clear(); return; }
        s = s.substr(a, b - a + 1);
    }
    std::vector<std::string> Tokenize(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string tok;
        while (ss >> tok) out.push_back(tok);
        return out;
    }
}

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
    auto sit = scenes_data.find(scene_name);
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    auto& reg = ComponentSerializerRegistry::Get();

    // Накопленные компоненты текущей сущности — собираем ВЕСЬ набор до создания
    // (нужна полная сигнатура архетипа), затем одна вставка (как CreateEntity).
    struct Pending { const ComponentSerializer* h; std::vector<std::string> tokens; };
    std::vector<Pending> comps;
    uint32_t cur_id = 0;
    bool     have_block = false;
    uint32_t synthetic = 0xFFFF0000u;                    // id для блоков без явного (back-compat)

    std::unordered_map<uint32_t, Entity> old_to_new;     // id из файла → новый Entity
    std::vector<Entity> created;

    auto flush = [&]() {
        if (!have_block) { comps.clear(); return; }
        have_block = false;
        if (comps.empty()) return;

        std::set<std::type_index> sig;
        for (auto& p : comps) sig.insert(p.h->sig_type);

        Archetype& arch = scene->archetypes[sig];
        Entity e = scene->next_entity_id++;
        arch.entities.push_back(e);
        for (auto& p : comps) p.h->load(arch, p.tokens);   // ensure_component<T> + дописать
        scene->entity_to_archetype[e] = &arch;
        scene->entity_to_index[e] = arch.entities.size() - 1;

        old_to_new[cur_id] = e;
        created.push_back(e);
        comps.clear();
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        Trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("[entity]", 0) == 0) {            // заголовок: [entity] <id>
            flush();
            std::vector<std::string> htoks = Tokenize(line.substr(8));
            cur_id = htoks.empty() ? synthetic++
                : static_cast<uint32_t>(std::strtoul(htoks[0].c_str(), nullptr, 10));
            have_block = true;
            continue;
        }

        if (!have_block) continue;

        const size_t eq = line.find('=');
        std::string name = (eq == std::string::npos) ? line : line.substr(0, eq);
        std::string rhs  = (eq == std::string::npos) ? std::string{} : line.substr(eq + 1);
        Trim(name);
        const ComponentSerializer* h = reg.ByName(name);
        if (!h) continue;                                // незнакомый компонент — пропускаем
        comps.push_back({ h, Tokenize(rhs) });
    }
    flush();

    // Проход 2: ParentComponent сейчас держит СТАРЫЙ id из файла. Ремапим в новый Entity
    // и заполняем обратный индекс scene->children (CreateEntity делает это при создании —
    // здесь путь иной, поэтому вручную). Родитель уже создан: все сущности есть после прохода 1.
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

    dirty_entity = true;
    return created;
}

