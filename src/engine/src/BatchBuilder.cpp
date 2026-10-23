#include "PCH.h"
#include "BatchBuilder.h"
#include "BaseComponents.h"
#include "RenderCommandData.h"
#include "RenderSnapshot.h"
#include "PipeManager.h"
#include "PassManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "BufferManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "ModelData.h"
#include "TextureData.h"
#include "EngineProfiler.h"
#include <bit>

using namespace BatchKeys;
using namespace ShaderBase;

struct MatSpLayout {
    std::shared_ptr<SDL_GPUGraphicsPipeline>  pipeline;
    std::vector<UVL_Block>                    uvl;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
    SlotWord                slot[MAX_SLOTS] = {};
    MatSpKey                res_key = 0;
    GpuResourceBatchKey     gpu_resource_key = 0;
    bool                    bindable = false;
    bool                    variative = false;
};


CommandBatchKey HashCommandBatchKey(ModelId model, uint32_t submesh_index) {
    CommandBatchKey key = std::hash<ModelId>{}(model);
    key ^= static_cast<CommandBatchKey>(submesh_index) + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);

    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;

    return key;
}

static inline uint64_t MixKey(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

const std::shared_ptr<std::vector<uint8_t>> kNoParams{};

MatSpKey HashMatSpResources(ShaderProgramId push_source, const ShaderProgram* sp,
                            const std::vector<uint8_t>* params, const SlotWord* slot_words,
                            const std::vector<const TextureHandle*>& block_handles) {
    if (!sp) {
        return 0xFFFFFFFFFFFFFFFFull;
    }
    MatSpKey key = 0;
    const size_t slot_count = std::min<size_t>(sp->required_slots.size(), MAX_SLOTS);
    for (size_t s = 0; s < slot_count; ++s) {
        key += static_cast<MatSpKey>(sp->required_slots[s]) + 0x9e3779b97f4a7c15ull;
        key ^= static_cast<MatSpKey>(slot_words[s].base)
             | (static_cast<MatSpKey>(slot_words[s].cell) << 16)
             | (static_cast<MatSpKey>(slot_words[s].count) << 24);
        key *= 0xff51afd7ed558ccd;
        key ^= key >> 29;
    }
    for (const TextureHandle* h : block_handles) {
        key ^= reinterpret_cast<MatSpKey>(h);
        key *= 0xff51afd7ed558ccd;
        key ^= key >> 29;
    }
    if (params && !params->empty()) key ^= reinterpret_cast<MatSpKey>(params);
    key = MixKey(key ^ (std::hash<ShaderProgramId>{}(push_source) + 0x9e3779b97f4a7c15ull));
    return MixKey(key);
}

TextureBatchKey HashTextureBatchKey(MatSpKey res_key, uint32_t material_index) {
    return MixKey(res_key ^ (static_cast<TextureBatchKey>(material_index) + 0x9e3779b97f4a7c15ull));
}

GpuResourceBatchKey HashGpuResourceBatchKey(const std::vector<BufferDataName>& vertex_storage_buffers,
                                            const std::vector<BufferDataName>& fragment_storage_buffers,
                                            const std::vector<const TextureHandle*>& block_handles) {
    GpuResourceBatchKey key = 0;
    auto mix = [&key](GpuResourceBatchKey v) {
        key ^= v;
        key *= 0xff51afd7ed558ccd;
        key ^= key >> 29;
    };
    auto mix_buffers = [&mix](const std::vector<BufferDataName>& names) {
        mix(names.size());
        for (BufferDataName n : names) mix(std::hash<std::string_view>{}(n));
    };
    mix_buffers(vertex_storage_buffers);
    mix_buffers(fragment_storage_buffers);
    for (const TextureHandle* h : block_handles) mix(reinterpret_cast<GpuResourceBatchKey>(h->atlas));
    return MixKey(key);
}

std::vector<BufferData*> ResolveBuffers(BufferManager* bm, const std::vector<BufferDataName>& names) {
    std::vector<BufferData*> out;
    out.reserve(names.size());
    for (BufferDataName n : names)
        if (BufferData* b = bm->GetBufferData(n)) out.push_back(b);
    return out;
}

ShaderBatchKey HashShaderBatchKey(const SDL_GPUGraphicsPipeline* pipeline) {
    return MixKey(reinterpret_cast<ShaderBatchKey>(pipeline));
}

BatchBuilder::BatchBuilder()
{
}

void BatchBuilder::SetDummyTexture(const std::string& name, TextureManager* tm)
{
    if (!tm) return;
    dummy_texture = tm->InternTexture(name);
    TextureHandle* handle = tm->GetTextureHandle(dummy_texture);
    if (!handle) return;
    TextureAtlas* atlas = handle->atlas;
    if (!atlas) return;

    if (atlas->texture_binding.texture && !(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "USAGE VIOLATION: dummy texture '%s' lives in atlas '%s', whose GPU texture was ALREADY "
            "CREATED without SDL_GPU_TEXTUREUSAGE_SAMPLER. It IS bound as a fragment sampler "
            "(fallback for missing material slots) - the bind will abort. "
            "Declare SAMPLER at atlas creation.",
            name.c_str(), atlas->name.c_str());
    }

    // Своего материала у dummy нет, поэтому SAMPLER его атласу объявляем здесь.
    atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
}

MatSpLayout BatchBuilder::BuildMatSpLayout(PipeManager* pm, PassManager* pass_manager, TextureManager* tm,
    ShaderManager* sm, const Material& material, const ShaderProgram& sp, ShaderProgramId sp_id,
    const std::shared_ptr<std::vector<uint8_t>>& sp_params) const
{
    TextureHandle* dummy = tm ? tm->GetTextureHandle(dummy_texture) : nullptr;
    if (dummy && !dummy->atlas) dummy = nullptr;

    const VariativeRoles cells = CollectVariativeRoles(material);
    std::vector<const TextureHandle*> block_handles;

    MatSpLayout lay{};
    // Промах молчаливый: программа без пайплайна (например, проход перенастроили без
    // пересборки пайплайнов) выпадает из отрисовки без строки в логе.
    lay.pipeline = pm->FindGraphicsPipeline(sp, sm, pass_manager);
    lay.bindable = true;
    lay.uvl.reserve(sp.required_slots.size());
    lay.texture_binding.reserve(sp.required_slots.size());

    for (size_t s = 0; s < sp.required_slots.size(); ++s) {
        const TextureSlotRole role = sp.required_slots[s];
        auto it = material.textures.find(role);
        const std::vector<TextureId>* names =
            (it != material.textures.end()) ? &it->second : nullptr;

        const uint32_t base = safe_u32(lay.uvl.size());

        TextureHandle* def = (names && !names->empty() && tm)
            ? tm->GetTextureHandle((*names)[0]) : nullptr;
        if (!def || !def->atlas) {
            def = dummy;
        }
        if (!def) { lay.bindable = false; break; }

        lay.uvl.push_back(MakeUVL(def->texture_data));
        block_handles.push_back(def);
        lay.texture_binding.push_back(def->atlas->texture_binding);

        uint32_t count = 1;
        uint32_t cell = 0;
        bool has_cell = false;
        for (uint32_t c = 0; c < cells.count; ++c)
            if (cells.role[c] == role) { cell = c; has_cell = true; break; }

        if (has_cell) {
            if (lay.uvl.size() + names->size() - 1 <= MAX_UVL_BLOCKS)
            for (size_t v = 1; v < names->size(); ++v) {
                TextureHandle* h = tm ? tm->GetTextureHandle((*names)[v]) : nullptr;
                if (!h || !h->atlas) continue;
                lay.uvl.push_back(MakeUVL(h->texture_data));
                block_handles.push_back(h);
                ++count;
            }
        }
        if (count == 1) cell = 0;
        else            lay.variative = true;

        assert((cells.count != 0 || (count == 1 && base == safe_u32(s)))
            && "BuildMatSpLayout: material without variants must yield the legacy UVL table");

        if (s < MAX_SLOTS)
            lay.slot[s] = { safe_u32_u8(count), safe_u32_u8(cell), safe_u32_u16(base) };
    }

    if (!lay.bindable) {
        lay.uvl.clear();
        lay.texture_binding.clear();
    }
    else {
        lay.res_key          = HashMatSpResources(sp_id, &sp, sp_params.get(), lay.slot, block_handles);
        lay.gpu_resource_key = HashGpuResourceBatchKey(sp.vertex_shader_buffer_names,
            sp.fragment_shader_buffer_names, block_handles);
    }
    return lay;
}

TextureBatch* BatchBuilder::ResolveTextureBatch(PassBatches& pass, ShaderProgram* sp, const ShaderName& sp_name,
    const std::shared_ptr<std::vector<uint8_t>>& sp_params, const MatSpLayout& lay, uint32_t section,
    ShaderManager* sm, BufferManager* bm, uint64_t& path_key)
{
    if (!lay.pipeline) return nullptr;

    auto& shader_map = pass.shader_batches;
    const ShaderBatchKey shader_key = HashShaderBatchKey(lay.pipeline.get());
    auto it = shader_map.find(shader_key);
    if (it == shader_map.end())
    {
        ShaderBatch new_batch{};
        new_batch.pipeline = lay.pipeline;
        if (VertexShaderData* vsd = sm->GetVertexShader(sp->vs_id)) {
            new_batch.vertexBuffers = ResolveBuffers(bm, vsd->vertex_buffer_names);
            if (vsd->index_buffer)
                new_batch.indexBuffer = bm->GetBufferData(vsd->index_buffer);
        }
        it = shader_map.emplace(shader_key, std::move(new_batch)).first;
    }

    auto& res_map = it->second.resource_batches;
    auto res_it = res_map.find(lay.gpu_resource_key);
    if (res_it == res_map.end()) {
        GpuResourceBatch new_res{};
        new_res.vertexStorageBuffers   = ResolveBuffers(bm, sp->vertex_shader_buffer_names);
        new_res.fragmentStorageBuffers = ResolveBuffers(bm, sp->fragment_shader_buffer_names);
        new_res.texture_binding        = lay.texture_binding;
        res_it = res_map.emplace(lay.gpu_resource_key, std::move(new_res)).first;
    }

    const TextureBatchKey tex_key = HashTextureBatchKey(lay.res_key, section);
    auto& tex_map = res_it->second.texture_batches;
    auto tex_it = tex_map.find(tex_key);
    if (tex_it == tex_map.end()) {
        TextureBatch new_texb{};
        new_texb.params = sp_params;
        new_texb.push_instructions = std::make_shared<const PushInstructions>(sm->CollectPushInstructions(sp_name));
        new_texb.texture_uvl = lay.uvl;
        std::copy(std::begin(lay.slot), std::end(lay.slot), std::begin(new_texb.variant_layout.slot));
        new_texb.variant_layout.material_index = section;
        tex_it = tex_map.emplace(tex_key, std::move(new_texb)).first;
    }

    path_key = MixKey(MixKey(MixKey(shader_key) ^ lay.gpu_resource_key) ^ tex_key);
    return &tex_it->second;
}

void BatchBuilder::ResolveRecipe(PipeManager* pm, PassManager* pass_manager, TextureManager* tm, ShaderManager* sm,
    BufferManager* bm, ModelManager* mdm, MaterialManager* mtm, const Recipe& recipe) {

    resolved_scratch.clear();
    group_scratch.clear();
    const std::vector<RenderPassStep*>& passes = pass_manager->GetOrderedRenderPasses();
    const ModelId root = recipe.model;
    const uint32_t lod_count = mdm ? std::min(mdm->LevelCount(root), MAX_LOD) : 1u;

    for (uint32_t lod = 0; lod < lod_count; ++lod) {
        const ModelId model_id = mdm ? mdm->LevelModel(root, lod) : root;
        const ModelData* model = mdm ? mdm->FindModel(model_id) : nullptr;
        if (!model) continue;

        for (uint32_t si = 0; si < model->submeshes.size(); ++si) {
            const SubMeshData& submesh = model->submeshes[si];
            if (submesh.indexCount == 0) continue;
            if (submesh.material_index >= recipe.parts.size()) continue;

            Material* material = mtm ? mtm->GetMaterial(recipe.parts[submesh.material_index][lod]) : nullptr;
            if (!material) continue;

            for (const SpBinding& binding : material->shader_programs) {
                const std::shared_ptr<std::vector<uint8_t>>& sp_params =
                    (binding.params && !binding.params->empty()) ? binding.params : kNoParams;
                ShaderProgram* sp = sm ? sm->GetShaderProgram(binding.sp) : nullptr;
                // Имя РЕАЛЬНО взятой программы: по нему резолвятся push-инструкции, и на фолбэк-ветке
                // с запрошенным именем программа получила бы чужие пуши.
                ShaderProgramId resolved_id = binding.sp;
                if (!sp) {
                    sp = sm ? sm->GetShaderProgram(fallback_sp) : nullptr;
                    if (!sp) continue;
                    resolved_id = fallback_sp;
                }
                RenderPassStep* rp = pass_manager->GetRenderPassStep(sp->render_pass_name);
                if (!rp || rp->ordinal >= passes.size() || passes[rp->ordinal] != rp) continue;
                const uint32_t pass_index = rp->ordinal;

                const MatSpLayout lay = BuildMatSpLayout(pm, pass_manager, tm, sm, *material, *sp, resolved_id, sp_params);
                if (!lay.bindable) continue;

                const uint32_t section = lay.variative ? StateSection(submesh.material_index, lod) : 0u;
                uint64_t path_key = 0;
                TextureBatch* tb = ResolveTextureBatch(pass_batches[pass_index], sp, sm->ShaderProgramNameOf(resolved_id),
                    sp_params, lay, section, sm, bm, path_key);
                if (!tb) continue;

                const uint64_t cmd_key = MixKey(MixKey(HashCommandBatchKey(model_id, si) + lod) ^ path_key);
                resolved_scratch.push_back({ pass_index, tb, &submesh, cmd_key, static_cast<uint8_t>(lod) });
            }
        }
    }

    const uint64_t root_key = MixKey(std::hash<ModelId>{}(root));

    for (size_t first = 0; first < resolved_scratch.size(); ++first) {
        const uint32_t pass = resolved_scratch[first].pass;
        bool seen = false;
        for (size_t i = 0; i < first && !seen; ++i) seen = resolved_scratch[i].pass == pass;
        if (seen) continue;

        uint64_t group_key = root_key;
        for (size_t i = first; i < resolved_scratch.size(); ++i)
            if (resolved_scratch[i].pass == pass) group_key = MixKey(group_key ^ resolved_scratch[i].key);
        group_scratch.push_back({ pass, group_key, static_cast<uint8_t>(lod_count), root });
    }
}

void BatchBuilder::InsertResolvedRecipe(uint32_t recipe_index)
{
    for (const ResolvedGroup& g : group_scratch) {
        std::vector<InstanceGroup>& pass_groups = pass_batches[g.pass].instance_groups;
        auto [index_it, created] = group_index_by_key[g.pass].try_emplace(g.key, safe_u32(pass_groups.size()));
        const uint32_t group_index = index_it->second;
        if (created) {
            InstanceGroup& group = pass_groups.emplace_back();
            group.lod_count = g.lod_count;
            group.root_model = g.root_model;
            for (const ResolvedCmd& c : resolved_scratch) {
                if (c.pass != g.pass) continue;
                auto [lit, leaf_created] = c.texture->command_batches.try_emplace(MixKey(g.key ^ c.key));
                if (leaf_created) {
                    CommandBatch& leaf = lit->second;
                    leaf.instance_group_index = group_index;
                    leaf.level = c.level;
                    leaf.submesh = { c.submesh->indexCount, c.submesh->indexOffset, c.submesh->vertexOffset };
                }
            }
        }
        group_table.group_of[size_t(recipe_index) * group_table.pass_count + g.pass] = group_index;
    }
}

void BatchBuilder::UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, DrawPopulation* population, SceneData* scene)
{
    if (!scene || !population) return;

    const bool resources_changed = dirty_batches.exchange(false);
    const bool population_changed = population->TakeChanged(om);
    if (!resources_changed && !population_changed) return;

    bool live_recipes_changed = false;
    {
        PROF_SCOPE(Sim, "  population_sweep");
        live_recipes_changed = population->Sweep(om, scene);
    }
    if (resources_changed || live_recipes_changed) {
        PROF_SCOPE(Sim, "  build_tree");
        BuildRenderBatches(pm, pass_manager, tm, sm, bm, mdm, mtm, *population);
    }
    {
        PROF_SCOPE(Sim, "  population_scatter");
        population->Scatter(om, scene, group_table);
    }
    {
        PROF_SCOPE(Sim, "  apply_population");
        ApplyPopulation(population->Ranges());
    }
    {
        PROF_SCOPE(Sim, "  build_layout_snapshot");
        BuildLayoutSnapshot(pass_manager, bm);
    }

    if (resources_changed) ++rebuild_epoch;
    ++batches_revision;
}

void BatchBuilder::BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, TextureManager* tm,
    ShaderManager* sm, BufferManager* bm, ModelManager* mdm, MaterialManager* mtm, const DrawPopulation& population)
{
    const std::vector<RenderPassStep*>& passes = pass_manager->GetOrderedRenderPasses();
    pass_batches.assign(passes.size(), {});

    const RecipeRegistry& recipes = population.Recipes();
    const std::vector<uint32_t>& recipe_counts = population.RecipeCounts();
    group_table.pass_count = safe_u32(passes.size());
    group_table.group_of.assign(size_t(recipes.Count()) * group_table.pass_count, kNoGroup);
    group_index_by_key.assign(passes.size(), {});

    for (uint32_t r = 0; r < recipes.Count(); ++r) {
        if (r >= recipe_counts.size() || recipe_counts[r] == 0) continue;
        ResolveRecipe(pm, pass_manager, tm, sm, bm, mdm, mtm, recipes.At(RecipeId{ safe_u32t_i(r) }));
        InsertResolvedRecipe(r);
    }

    std::vector<uint32_t> pass_group_base(passes.size());
    uint32_t group_count = 0;
    for (size_t p = 0; p < passes.size(); ++p) {
        pass_group_base[p] = group_count;
        group_count += safe_u32(pass_batches[p].instance_groups.size());
    }
    group_table.group_count = group_count;
    for (size_t cell = 0; cell < group_table.group_of.size(); ++cell) {
        uint32_t& group = group_table.group_of[cell];
        if (group != kNoGroup) group += pass_group_base[cell % group_table.pass_count];
    }
}

void BatchBuilder::ApplyPopulation(const std::vector<GroupRange>& ranges)
{
    assert(ranges.size() == group_table.group_count && "ApplyPopulation: population scattered against another group table");
    uint32_t offset = 0;
    uint32_t group_index = 0;

    for (PassBatches& pass : pass_batches) {
        for (InstanceGroup& group : pass.instance_groups) {
            const GroupRange& range = ranges[group_index++];
            assert(range.first == offset && "ApplyPopulation: PIB order of DrawPopulation differs from the pass walk");
            group.pib_first = range.first;
            group.size = range.size;
            offset += range.size;
        }
        for (auto& [_, shader_batch] : pass.shader_batches)
            for (auto& [_, res_batch] : shader_batch.resource_batches)
                for (auto& [_, texture_batch] : res_batch.texture_batches)
                    for (auto& [_, leaf] : texture_batch.command_batches) {
                        const InstanceGroup& group = pass.instance_groups[leaf.instance_group_index];
                        leaf.firstInstance = group.pib_first;
                        leaf.instanceCount = leaf.level == 0 ? group.size : 0u;
                    }
    }
}

void BatchBuilder::BuildLayoutSnapshot(PassManager* pass_manager, BufferManager* bm)
{
    const std::vector<RenderPassStep*>& passes = pass_manager->GetOrderedRenderPasses();
    auto layout = std::make_shared<RenderSnap::BatchLayout>();
    layout->passes.reserve(passes.size());
    layout->indirectBuffer = bm->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER);

    static const PassBatches no_batches{};
    for (size_t p = 0; p < passes.size(); ++p)
    {
        const RenderPassStep* rp = passes[p];
        const PassBatches& pass = p < pass_batches.size() ? pass_batches[p] : no_batches;

        RenderSnap::PassBatchSnap pass_snap;
        pass_snap.shader_batches.reserve(pass.shader_batches.size());
        pass_snap.global_texture_bindings.reserve(rp->global_texture_bindings.size());

        for (TextureAtlas* atlas : rp->global_texture_bindings) {
            if (!atlas || !atlas->texture_binding.texture) {
                SDL_Log("BatchBuilder::BuildLayoutSnapshot missing GPU texture in pass global_texture_bindings");
                continue;
            }
            pass_snap.global_texture_bindings.push_back(atlas->texture_binding);
        }

        uint32_t pass_cmd_index = 0;
        for (const auto& [shader_key, shader_batch] : pass.shader_batches)
        {
            RenderSnap::ShaderBatchSnap shader_snap;
            shader_snap.pipeline = shader_batch.pipeline;
            shader_snap.vertexBuffers = shader_batch.vertexBuffers;
            shader_snap.indexBuffer = shader_batch.indexBuffer;
            shader_snap.resource_batches.reserve(shader_batch.resource_batches.size());

            for (const auto& [res_key, res_batch] : shader_batch.resource_batches)
            {
                RenderSnap::GpuResourceBatchSnap res_snap;
                res_snap.vertexStorageBuffers = res_batch.vertexStorageBuffers;
                res_snap.fragmentStorageBuffers = res_batch.fragmentStorageBuffers;
                res_snap.texture_binding = res_batch.texture_binding;
                res_snap.texture_batches.reserve(res_batch.texture_batches.size());

                for (const auto& [texture_key, texture_batch] : res_batch.texture_batches)
                {
                    RenderSnap::TextureBatchSnap texture_snap;
                    texture_snap.texture_uvl = texture_batch.texture_uvl;
                    texture_snap.variant_layout = texture_batch.variant_layout;
                    texture_snap.params = texture_batch.params;
                    texture_snap.push_instructions = texture_batch.push_instructions;
                    texture_snap.indirect_command_index = pass_cmd_index;
                    texture_snap.draw_count = safe_u32(texture_batch.command_batches.size());
                    pass_cmd_index += texture_snap.draw_count;
                    res_snap.texture_batches.push_back(std::move(texture_snap));
                }
                shader_snap.resource_batches.push_back(std::move(res_snap));
            }
            pass_snap.shader_batches.push_back(std::move(shader_snap));
        }
        pass_snap.num_commands = pass_cmd_index;
        layout->passes.push_back(std::move(pass_snap));
    }
    current_layout = std::move(layout);
}

void BatchBuilder::StampLayoutSnapshot(uint8_t slot)
{
    slot_layouts[slot] = current_layout;
}

void BatchBuilder::StampComputeSnapshot(uint8_t slot)
{
    slot_compute_layouts[slot] = current_compute_layout;
}

void BatchBuilder::BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm,
    BufferManager* bm, TextureManager* tm) {
    if (!sm || !sm->IsDirtyComputeBatches()) return;

    auto layout = std::make_shared<RenderSnap::ComputeLayout>();
    layout->passes.resize(pass_manager->GetOrderedComputePrepasses().size() + pass_manager->GetOrderedComputePasses().size());

    for (int32_t ci = 0; ci < sm->ComputePrograms().Count(); ++ci) {
        const ComputeProgramCell& slot = sm->ComputePrograms().At(ci);
        ComputeShaderProgram* sp = slot.object.get();
        if (!sp) continue;
        auto pipe = pm->GetComputePipeline(sp);
        if (!pipe) continue;

        ComputePassStep* cmp = pass_manager->GetComputePassStep(sp->compute_pass_name);
        // Пассы и препассы делят пространство имён (см. PassManager::IsPassNameTaken), поэтому
        // перебор «сначала пасс, потом препасс» однозначен.
        if (!cmp) cmp = pass_manager->GetComputePrepassStep(sp->compute_pass_name);
        if (!cmp || cmp->ordinal >= layout->passes.size()) continue;

        auto resolve_buffers = [&](const std::vector<BufferDataName>& names, const char* kind) {
            std::vector<BufferData*> out;
            out.reserve(names.size());
            for (BufferDataName n : names) {
                BufferData* bd = bm ? bm->GetBufferData(n) : nullptr;
                if (!bd) { SDL_Log("BuildComputeBatches '%s': %s storage buffer '%s' not found - binding slots will shift", slot.name.c_str(), kind, n); continue; }
                out.push_back(bd);
            }
            return out;
        };
        auto resolve_atlases = [&](const std::vector<AtlasId>& ids, const char* kind) {
            std::vector<TextureAtlas*> out;
            out.reserve(ids.size());
            for (AtlasId id : ids) {
                TextureAtlas* a = tm ? tm->GetTextureAtlas(id) : nullptr;
                if (!a) { SDL_Log("BuildComputeBatches '%s': %s atlas '%s' not found - binding slots will shift", slot.name.c_str(), kind, tm ? tm->AtlasNameOf(id).c_str() : "?"); continue; }
                out.push_back(a);
            }
            return out;
        };

        RenderSnap::ComputeDispatch dispatch{};
        dispatch.pipeline = pipe;
        dispatch.rw_storage_buffers = resolve_buffers(sp->rw_storage_buffer_names, "rw");
        dispatch.ro_storage_buffers = resolve_buffers(sp->ro_storage_buffer_names, "ro");

        dispatch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            TextureAtlas* a = tm ? tm->GetTextureAtlas(d.texture_atlas) : nullptr;
            if (!a) { SDL_Log("BuildComputeBatches '%s': rw atlas '%s' not found - binding slots will shift", slot.name.c_str(), tm ? tm->AtlasNameOf(d.texture_atlas).c_str() : "?"); continue; }
            dispatch.rw_storage_textures.push_back({ a, d.mip_level, d.layer });
        }
        dispatch.ro_storage_textures = resolve_atlases(sp->ro_storage_texture_ids, "ro");
        dispatch.texture_binding     = resolve_atlases(sp->texture_sampler_ids, "sampler");
        dispatch.push_instructions = sm->CollectComputePushInstructions(slot.name);
        dispatch.dispatch_func = sm->GetDispatchInstruction(slot.name);

        ComputeShaderData* csd = sm->GetComputeShader(sp->cs_id);
        if (!csd)
            SDL_Log("BuildComputeBatches '%s': compute shader '%s' not found in registry - dispatch falls back to 1x1x1",
                slot.name.c_str(), sm->ComputeShaders().NameOf(sp->cs_id).c_str());
        dispatch.threadcount_x = csd ? csd->threadcount_x : 1u;
        dispatch.threadcount_y = csd ? csd->threadcount_y : 1u;
        dispatch.threadcount_z = csd ? csd->threadcount_z : 1u;

        layout->passes[cmp->ordinal].push_back(std::move(dispatch));
    }
    current_compute_layout = std::move(layout);
    sm->SetDirtyComputeBatches(false);
}
