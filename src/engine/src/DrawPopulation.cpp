#include "PCH.h"
#include "DrawPopulation.h"
#include "BaseComponents.h"
#include "ObjectManager.h"
#include "RenderCommandData.h"

static inline uint64_t MixRecipeWord(uint64_t key, uint64_t word)
{
    key ^= word + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
    return key;
}

static uint64_t HashRecipe(ModelId model, const std::vector<MaterialSlot>& parts)
{
    uint64_t key = MixRecipeWord(parts.size(), std::hash<ModelId>{}(model));
    for (const MaterialSlot& part : parts)
        for (uint32_t L = 0; L < MAX_LOD; ++L)
            key = MixRecipeWord(key, std::hash<MaterialId>{}(part.per_lod[L]));
    return key;
}

static bool SameRecipe(const Recipe& recipe, ModelId model, const std::vector<MaterialSlot>& parts)
{
    if (recipe.model != model || recipe.parts.size() != parts.size()) return false;
    for (size_t k = 0; k < parts.size(); ++k)
        for (uint32_t L = 0; L < MAX_LOD; ++L)
            if (recipe.parts[k][L] != parts[k].per_lod[L]) return false;
    return true;
}

RecipeId RecipeRegistry::Intern(ModelId model, const std::vector<MaterialSlot>& parts)
{
    const uint64_t hash = HashRecipe(model, parts);
    const auto [first, last] = by_hash.equal_range(hash);
    for (auto it = first; it != last; ++it)
        if (SameRecipe(At(it->second), model, parts)) return it->second;

    Recipe& recipe = recipes.emplace_back();
    recipe.model = model;
    recipe.parts.resize(parts.size());
    for (size_t k = 0; k < parts.size(); ++k)
        for (uint32_t L = 0; L < MAX_LOD; ++L)
            recipe.parts[k][L] = parts[k].per_lod[L];

    const RecipeId id{ safe_size_i(recipes.size() - 1) };
    by_hash.emplace(hash, id);
    return id;
}

bool DrawPopulation::TakeChanged(const ObjectManager* om)
{
    const bool marked = dirty.exchange(false);
    const uint64_t entity_revision = om->EntityRevision();
    const bool entities_changed = entity_revision != seen_entity_revision;
    seen_entity_revision = entity_revision;
    return marked || entities_changed;
}

static void CountVisibleRecipes(Renderable& rend, RecipeRegistry& recipes, std::vector<uint32_t>& recipe_counts)
{
    for (size_t i = 0; i < rend.size(); ++i) {
        if (!rend.visible[i]) continue;
        RecipeId& recipe = rend.recipe[i];
        if (!recipe) recipe = recipes.Intern(rend.model[i], rend.materials[i]);
        const uint32_t recipe_index = safe_i_u32(recipe.v);
        if (recipe_index >= recipe_counts.size()) recipe_counts.resize(recipe_index + 1, 0u);
        ++recipe_counts[recipe_index];
    }
}

bool DrawPopulation::Sweep(ObjectManager* om, SceneData* scene)
{
    recipe_counts.assign(recipes.Count(), 0u);

    om->ForEachArchetype<Renderable>(scene, [&](ComponentArray<Renderable>* renderables) {
        CountVisibleRecipes(renderables->data, recipes, recipe_counts);
    });

    bool live_changed = false;
    recipe_live.resize(recipe_counts.size(), 0);
    for (size_t r = 0; r < recipe_counts.size(); ++r) {
        const uint8_t live = recipe_counts[r] != 0 ? 1 : 0;
        if (recipe_live[r] != live) live_changed = true;
        recipe_live[r] = live;
    }
    return live_changed;
}

static void ScatterArchetype(const Renderable& rend, bool has_rows, uint32_t row_base,
                             const InstanceGroupTable& groups, size_t table_recipes,
                             uint32_t* group_cursor, uint32_t* pib_rows)
{
    const uint32_t pass_count = groups.pass_count;
    for (size_t i = 0; i < rend.size(); ++i) {
        if (!rend.visible[i]) continue;
        assert(rend.recipe[i] && "DrawPopulation::Scatter: visible entity without recipe - Sweep must run first");
        const size_t recipe_index = safe_i_u32(rend.recipe[i].v);
        if (recipe_index >= table_recipes) continue;

        const uint32_t row = has_rows ? row_base + safe_u32(i) : kPibNoRow;
        const uint32_t* group_of = &groups.group_of[recipe_index * pass_count];
        for (uint32_t p = 0; p < pass_count; ++p)
            if (group_of[p] != kNoGroup) pib_rows[group_cursor[group_of[p]]++] = row;
    }
}

void DrawPopulation::Scatter(ObjectManager* om, SceneData* scene, const InstanceGroupTable& groups)
{
    const uint32_t pass_count = groups.pass_count;
    const size_t table_recipes = pass_count ? groups.group_of.size() / pass_count : 0;

    ranges.assign(groups.group_count, GroupRange{});
    for (size_t r = 0; r < recipe_counts.size() && r < table_recipes; ++r) {
        if (recipe_counts[r] == 0) continue;
        const uint32_t* group_of = &groups.group_of[r * pass_count];
        for (uint32_t p = 0; p < pass_count; ++p)
            if (group_of[p] != kNoGroup) ranges[group_of[p]].size += recipe_counts[r];
    }

    uint32_t total = 0;
    group_cursor.resize(ranges.size());
    for (size_t g = 0; g < ranges.size(); ++g) {
        ranges[g].first = total;
        group_cursor[g] = total;
        total += ranges[g].size;
    }
    pib_rows.resize(total);

    struct ArchetypeRowBase {
        const Renderable* column;
        uint32_t          base;
    };
    std::vector<ArchetypeRowBase> row_bases;
    uint32_t row_count = 0;
    // Отбор архетипов и их порядок обязаны совпадать с TransformDataModule: строка трансформа =
    // число строк архетипов до этого + индекс сущности в нём.
    om->ForEachArchetype<Positions, Renderable>(scene,
        [&](ComponentArray<Positions>* positions, ComponentArray<Renderable>* renderables) {
            row_bases.push_back({ &renderables->data, row_count });
            row_count += safe_u32(positions->size());
        });

    om->ForEachArchetype<Renderable>(scene, [&](ComponentArray<Renderable>* renderables) {
        const Renderable& rend = renderables->data;
        const auto row_base = std::find_if(row_bases.begin(), row_bases.end(),
            [&](const ArchetypeRowBase& archetype) { return archetype.column == &rend; });
        const bool has_rows = row_base != row_bases.end();
        ScatterArchetype(rend, has_rows, has_rows ? row_base->base : 0, groups, table_recipes,
                         group_cursor.data(), pib_rows.data());
    });
}
