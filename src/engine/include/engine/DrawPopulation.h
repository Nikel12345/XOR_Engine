#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "Utils.h"
#include "ResourceId.h"
#include "BaseComponents.h"

class ObjectManager;
struct SceneData;

struct Recipe {
    ModelId model;
    std::vector<std::array<MaterialId, MAX_LOD>> parts;
};

class RecipeRegistry {
public:
    RecipeId Intern(ModelId model, const std::vector<MaterialSlot>& parts);
    const Recipe& At(RecipeId id) const { return recipes[safe_i_u32(id.v)]; }
    uint32_t Count() const { return safe_u32(recipes.size()); }

private:
    std::vector<Recipe> recipes;
    std::unordered_multimap<uint64_t, RecipeId> by_hash;
};

inline constexpr uint32_t kNoGroup = 0xFFFFFFFFu;

struct InstanceGroupTable {
    uint32_t pass_count = 0;
    uint32_t group_count = 0;
    // [рецепт * pass_count + проход] -> сквозной номер группы в порядке PIB; kNoGroup — рецепт в
    // проходе не рисуется.
    std::vector<uint32_t> group_of;
};

struct GroupRange {
    uint32_t first = 0;
    uint32_t size = 0;
};

class DrawPopulation {
public:
    void MarkDirty() { dirty = true; }
    bool TakeChanged(const ObjectManager* om);

    bool Sweep(ObjectManager* om, SceneData* scene);
    void Scatter(ObjectManager* om, SceneData* scene, const InstanceGroupTable& groups);

    const RecipeRegistry& Recipes() const { return recipes; }
    const std::vector<uint32_t>& RecipeCounts() const { return recipe_counts; }
    const std::vector<GroupRange>& Ranges() const { return ranges; }
    const std::vector<uint32_t>& PibRows() const { return pib_rows; }

private:
    RecipeRegistry recipes;
    std::vector<uint32_t> recipe_counts;
    std::vector<uint8_t> recipe_live;

    std::vector<GroupRange> ranges;
    std::vector<uint32_t> group_cursor;
    std::vector<uint32_t> pib_rows;

    std::atomic<bool> dirty{ true };
    uint64_t seen_entity_revision = ~0ull;
};
