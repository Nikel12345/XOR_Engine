#include "PCH.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"
#include <algorithm>
#include <new>
#include <set>
#include <unordered_map>

SceneData* ObjectManager::CreateScene(const SceneName& name) {
    // Пустое имя — не сцена, а СЛЕД её отсутствия: так GetActiveSceneName сообщает «активной нет»,
    // и это значение доезжает сюда через UI-команды.
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

std::vector<uint8_t> ObjectManager::SaveScene(SceneData* scene)
{
    if (!scene) return {};
    auto& reg = ComponentSpecRegistry::Get();

    struct Block {
        std::string                       key;
        Archetype*                        arch;
        std::vector<const ComponentSpec*> specs;
    };
    std::vector<Block> blocks;
    for (auto& [sig, arch] : scene->archetypes) {
        // Производные сущности в файл не идут — их пересоздаст генератор сцены.
        if (arch.components.count(std::type_index(typeid(GeneratedComponent)))) continue;
        if (arch.entities.empty()) continue;
        Block b{ {}, &arch, {} };
        for (auto& [tindex, arr] : arch.components)
            if (const ComponentSpec* h = reg.ByType(tindex)) b.specs.push_back(h);
        if (b.specs.empty()) continue;
        std::sort(b.specs.begin(), b.specs.end(),
                  [](const ComponentSpec* x, const ComponentSpec* y) { return x->name < y->name; });
        for (size_t i = 0; i < b.specs.size(); ++i) { if (i) b.key += ','; b.key += b.specs[i]->name; }
        blocks.push_back(std::move(b));
    }
    // Порядок обхода scene->archetypes — порядок std::type_index, стандартом он не определён:
    // без сортировки пересохранение той же сцены тасовало бы таблицы.
    std::sort(blocks.begin(), blocks.end(), [](const Block& x, const Block& y) { return x.key < y.key; });

    // Parent в файле — сквозной номер строки, а его задаёт ровно этот порядок блоков.
    std::unordered_map<Entity, uint32_t> row_of;
    uint32_t base = 0;
    for (const Block& b : blocks) {
        for (size_t i = 0; i < b.arch->entities.size(); ++i) row_of[b.arch->entities[i]] = base + safe_u32(i);
        base += safe_u32(b.arch->entities.size());
    }

    sheaf::Writer w;
    uint32_t unresolved = 0;
    base = 0;
    for (const Block& b : blocks) {
        sheaf::Table t;
        t.rows = safe_u32(b.arch->entities.size());
        for (const ComponentSpec* h : b.specs) {
            sheaf::Component& comp = t.components.emplace_back();
            comp.name = h->name;
            h->Save(*b.arch, t.rows, w, comp.fields);
            if (h->sig_type != std::type_index(typeid(ParentComponent))) continue;
            for (sheaf::Column& col : comp.fields) {
                if (col.name != "parent") continue;
                col.type = sheaf::Type::Ref;
                for (uint32_t i = 0; i < t.rows; ++i) {
                    auto it = row_of.find(col.values[i]);
                    if (it != row_of.end()) { col.values[i] = it->second; continue; }
                    col.values[i] = base + i;      // родитель не сохраняется — как и в LoadScene, самоссылка
                    ++unresolved;
                }
            }
        }
        base += t.rows;
        w.Add(std::move(t));
    }
    if (unresolved)
        SDL_Log("SaveScene: %u parent links point outside the saved entities - written as self", unresolved);
    return w.Finish();
}

namespace {

constexpr Entity kNoEntity = static_cast<Entity>(-1);

// Таблицы приходят из разбора по одной: строки компонентов заводятся до данных таблицы, и числовые
// поля читатель кладёт прямо в колонки ECS.
class SceneLoader final : public sheaf::TableVisitor {
public:
    explicit SceneLoader(SceneData* scene) : scene_(scene) {}

    std::vector<Entity>   created;
    std::vector<Entity>   by_row;     // сквозной номер строки файла → новый Entity
    std::set<std::string> unknown;
    // Куски created из таблиц, где у известного компонента есть поле ref: ссылки переводятся
    // только в них.
    std::vector<std::pair<size_t, size_t>> with_refs;

    void Begin(const sheaf::Table& t, std::span<const std::string>, std::span<sheaf::Destination> dests) override
    {
        const size_t base = by_row.size();
        // Строки таблиц без единого известного компонента остаются kNoEntity: ссылка на них не резолвится.
        by_row.resize(base + t.rows, kNoEntity);
        specs_.clear();
        arch_ = nullptr;

        std::set<std::type_index> sig;
        size_t first = 0;
        for (const sheaf::Component& c : t.components) {
            if (const ComponentSpec* h = ComponentSpecRegistry::Get().ByName(c.name)) {
                specs_.push_back({ h, &c, first });
                sig.insert(h->sig_type);
            } else {
                unknown.insert(c.name);
            }
            first += c.fields.size();
        }
        if (t.rows == 0 || specs_.empty()) return;

        arch_ = &scene_->archetypes[sig];
        arch_->reserve(arch_->entities.size() + t.rows);
        ReserveRows(created, created.size() + t.rows);
        scene_->entity_to_archetype.reserve(scene_->entity_to_archetype.size() + t.rows);
        scene_->entity_to_index.reserve(scene_->entity_to_index.size() + t.rows);

        // Сущности создаём ДО строк компонентов: BeginLoad дописывает строки в хвост и считает
        // базу от entities.size() — порядок add обязан совпасть с arch.entities.
        for (size_t i = 0; i < t.rows; ++i) {
            const Entity e = scene_->next_entity_id++;
            arch_->entities.push_back(e);
            scene_->entity_to_archetype[e] = arch_;
            scene_->entity_to_index[e] = arch_->entities.size() - 1;
            by_row[base + i] = e;
            created.push_back(e);
        }
        const bool refs = std::ranges::any_of(specs_, [](const Spec& s) {
            return std::ranges::any_of(s.header->fields, [](const sheaf::Column& c) { return c.type == sheaf::Type::Ref; });
        });
        if (refs) with_refs.emplace_back(created.size() - t.rows, t.rows);
        for (const Spec& s : specs_)
            s.spec->BeginLoad(*arch_, *s.header, t.rows, dests.subspan(s.first_field, s.header->fields.size()));
    }

    void End(const sheaf::Table& t, std::span<const std::string> strings) override
    {
        if (!arch_) return;
        // header — компонент Begin, в той же таблице: адрес у него тот же.
        for (const Spec& s : specs_) s.spec->FinishLoad(*arch_, *s.header, t.rows, strings);
    }

private:
    struct Spec {
        const ComponentSpec*    spec;
        const sheaf::Component* header;
        size_t                  first_field;   // номер первого поля компонента в dests
    };

    SceneData*        scene_;
    Archetype*        arch_ = nullptr;
    std::vector<Spec> specs_;
};

} // namespace

std::expected<std::vector<Entity>, std::string> ObjectManager::LoadScene(const SceneName& scene_name, std::span<const uint8_t> bytes)
{
    auto sit = scenes_data.find(scene_name);
    // Автосоздание по имени — штатный путь первой загрузки, отдельный CreateScene игре не нужен.
    // Пустое имя при этом отвергает сам CreateScene (см. там) — отсюда проверка на null ниже.
    SceneData* scene = (sit != scenes_data.end()) ? sit->second.get()
                                                  : CreateScene(scene_name);
    if (!scene) return std::unexpected("no scene for name '" + scene_name + "'");

    const auto t_pass1 = Prof::Clock::now();
    SceneLoader loader(scene);
    std::expected<void, std::string> read;
    try {
        read = sheaf::Read(bytes, loader);
    } catch (const std::bad_alloc&) {
        read = std::unexpected(std::string("out of memory - object count in the file is corrupted?"));
    }
    if (!read) {
        // Таблицы до ошибки уже залиты: наполовину загруженной сцена не остаётся.
        scene->clear();
        ++entity_revision;
        return std::unexpected("scene.sheaf: " + read.error() + " - scene cleared");
    }
    const double pass1_ms = Prof::MsSince(t_pass1);
    const std::vector<Entity>& by_row = loader.by_row;

    // Проход 2: в ParentComponent лежит сквозной номер строки — меняем его на настоящий Entity и
    // заполняем scene->children. Отдельным проходом, потому что родитель мог приехать позже
    // ребёнка; после прохода 1 все сущности уже есть, так что ни глубина, ни циклы не важны.
    const auto t_pass2 = Prof::Clock::now();
    uint32_t unresolved = 0;
    for (const auto& [first, count] : loader.with_refs)
    for (size_t i = first; i < first + count; ++i) {
        const Entity e = loader.created[i];
        if (!Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = GetComponent<ParentComponent>(scene, e);
        const Entity parent = pc.parent < by_row.size() ? by_row[pc.parent] : kNoEntity;
        // Самоссылку пишет SaveScene, когда родитель не сохранялся. В children её класть нельзя:
        // объект стал бы своим ребёнком.
        if (parent == kNoEntity || parent == e) {
            if (parent == kNoEntity) ++unresolved;
            pc.parent = e;
            continue;
        }
        pc.parent = parent;
        scene->children[parent].push_back(e);
    }
    const double pass2_ms = Prof::MsSince(t_pass2);

    SDL_Log("  ObjectManager::LoadScene: pass1(parse+build)=%.1f  pass2(parent remap)=%.1f ms  [%zu ent, %zu archetypes]",
        pass1_ms, pass2_ms, loader.created.size(), scene->archetypes.size());
    for (const std::string& n : loader.unknown)
        SDL_Log("LoadScene: component '%s' is not registered - its columns dropped", n.c_str());
    if (unresolved)
        SDL_Log("LoadScene: %u parent links point to objects that were not loaded - hierarchy dropped", unresolved);

    ++entity_revision;
    return std::move(loader.created);
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

    for (const ComponentSpec* s : specs) s->LoadDefaults(arch, 1);

    return e;
}

