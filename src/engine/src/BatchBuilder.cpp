#include "PCH.h"
#include "BatchBuilder.h"
#include "RenderCommandData.h"
#include "ObjectManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "ShaderManager.h"
#include "ModelData.h"
#include "TextureData.h"
#include <unordered_set>


using namespace BatchKeys;

ModelBatchKey HashModelBatchKey(SubMeshData* submash) {
    if (!submash) {
        SDL_Log("HashModelBatchKey: model is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ModelBatchKey key = 0;
    key ^= reinterpret_cast<ModelBatchKey>(submash);

    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;

    return key;
}

// Ключ texture-батча = ИДЕНТИЧНОСТЬ материала через &Material::params (стабильный адрес
// под unique_ptr). Так материалы с одинаковыми текстурами, но разными params не
// схлопываются в один батч (как раньше молча терялась alpha), а мутация params на месте
// НЕ меняет ключ → фактор-твики в рантайме не вызывают перестройку дерева батчей.
TextureBatchKey HashTextureBatchKey(const Material* mat) {
    if (!mat) {
        SDL_Log("HashTextureBatchKey: material is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    TextureBatchKey key = reinterpret_cast<TextureBatchKey>(&mat->params);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;

}

AtlasBatchKey HashAtlasBatchKey(const Material* mat) {
    if (!mat) {
        SDL_Log("HashAtlasBatchKey: material is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    AtlasBatchKey key = 0;
    //if (mat->albedo) {
    //    key ^= reinterpret_cast<AtlasBatchKey>(mat->albedo->atlas);
    //}
    //if (mat->normal_texture) {
    //    key ^= reinterpret_cast<AtlasBatchKey>(mat->normal_texture->atlas) * 2654435761ull;
    //}
    for (const auto& [slot, tex] : mat->textures) {
        if (tex && tex->atlas) {
            key ^= reinterpret_cast<AtlasBatchKey>(tex->atlas) * (2654435761ull + static_cast<uint64_t>(slot));
        }
    }
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;

    return key;
}

ShaderBatchKey HashShaderBatchKey(ShaderProgram* sp) {
    if (!sp) {
        SDL_Log("HashShaderBatchKey: ShaderProgram is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ShaderBatchKey key = 0;
    key ^= reinterpret_cast<ShaderBatchKey>(sp);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

ShaderBatchKey HashShaderBatchKey(ComputeShaderProgram* sp) {
    if (!sp) {
        SDL_Log("HashShaderBatchKey: ShaderProgram is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ShaderBatchKey key = 0;
    key ^= reinterpret_cast<ShaderBatchKey>(sp);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

BatchBuilder::BatchBuilder()
{
}

void BatchBuilder::QueueCreate(Entity entity)
{
    std::lock_guard<std::mutex> lock(delta_mutex);
    entities_to_create.push_back(entity);
}

void BatchBuilder::QueueDelete(Entity entity)
{
    std::lock_guard<std::mutex> lock(delta_mutex);
    entities_to_delete.push_back(entity);
}

void BatchBuilder::AddEntityToBatches(Entity entity, PipeManager* pm,
    const MaterialComponent& material_component, const ModelComponent& model_component) {

    for (SubMeshData& submesh : model_component.model->submeshes)
    {
        if (material_component.materials.size() != model_component.model->submeshes.size()) {
            SDL_Log("BulidBatches:: Submash and material sizes mismatch");
        }
        Material* material = material_component.materials[submesh.material_index];

        for (ShaderProgram* sp : material->shader_programs)
        {
            if (!sp) {
                SDL_Log("BatchBuilder::Using non existing shader program in material");
                continue;
            }
            RenderPassStep* rp = sp->associated_render_pass;
            if (!rp) continue;

            auto& shader_map = rp->shader_batches;
            auto sp_key = HashShaderBatchKey(sp);
            auto it = shader_map.find(sp_key);
            if (it == shader_map.end())
            {
                ShaderBatchData new_batch{};
                new_batch.push_func = sp->push_func;
                new_batch.pipeline = pm->GetGraphicPipeline(sp);
                new_batch.vertexStorageBuffers = sp->vertex_shader_buffers;
                new_batch.fragmentStorageBuffers = sp->fragment_shader_buffers;
                shader_map[sp_key] = std::move(new_batch);
            }

            ShaderBatchData& sb = shader_map[sp_key];

            AtlasBatchKey atlas_key = sp->required_slots.empty() ? 0 : HashAtlasBatchKey(material);

            auto& atlas_map = sb.atlases_batches;
            auto atlas_it = atlas_map.find(atlas_key);
            if (atlas_it == atlas_map.end())
            {
                AtlasBatchData new_tex{};

                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    if (it == material->textures.end() || !it->second || !it->second->atlas) {
                        SDL_Log("BuildBatches:: Material is missing required atlas for shader slot");
                        assert(false && "Material is missing required texture for shader slot!");
                        continue;
                    }
                    new_tex.texture_binding.push_back(it->second->atlas->texture_binding);

                }

                atlas_map[atlas_key] = std::move(new_tex);
            }

            AtlasBatchData& atlas_batch = atlas_map[atlas_key];

            // Ключуем по идентичности материала ВСЕГДА (даже без текстур): материал = свой
            // батч, чтобы его params/alpha не делились с другим материалом того же шейдера.
            TextureBatchKey tex_key = HashTextureBatchKey(material);

            auto& tex_map = atlas_batch.texture_batches;
            auto texb_it = tex_map.find(tex_key);
            if (texb_it == tex_map.end()) {
                TextureBatchData new_texb{};
                // Факторы — только шейдерам, потребляющим материал (есть текстурные слоты). Геометрия-
                // only проходы (shadow: required_slots пуст) их не читают → не пушим, иначе пуш в
                // необъявленный uniform-слот этого пасса.
                new_texb.params = sp->required_slots.empty() ? nullptr : &material->params;
                new_texb.texture_uvl.reserve(material->textures.size());
                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    if (it == material->textures.end() || !it->second || !it->second->texture_data) {
                        SDL_Log("BuildBatches:: Material is missing required texture_data for shader slot");
                        assert(false && "Material is missing required texture for shader slot!");
                        continue;
                    }
                    new_texb.texture_uvl.push_back(*it->second->texture_data);   // КОПИЯ значения (см. инвариант в TextureBatchData)
                }

                tex_map[tex_key] = std::move(new_texb);
            }

            TextureBatchData& tex_batch = tex_map[tex_key];
            ModelBatchKey model_key = HashModelBatchKey(&submesh);

            auto& model_map = tex_batch.model_batches;
            auto model_it = model_map.find(model_key);
            if (model_it == model_map.end())
            {
                ModelBatchData new_model{};
                new_model.submesh = &submesh;
                new_model.instanceCount = 0;
                new_model.pib_sub_buffer.reserve(16);  // ��������� ������
                model_map[model_key] = std::move(new_model);
            }

            ModelBatchData& model_batch = model_map[model_key];

            uint32_t slot_index = safe_u32(model_batch.pib_sub_buffer.size());
            model_batch.instanceCount++;
            model_batch.pib_sub_buffer.push_back(entity);  // stable Entity id
            entity_slots[entity].push_back({ &model_batch, slot_index });

        }
    }

}

void BatchBuilder::RemoveEntityFromBatches(Entity entity)
{
    auto it = entity_slots.find(entity);
    if (it == entity_slots.end()) return;  // entity was never batched

    for (const PibSlot& slot : it->second) {
        ModelBatchData* model_batch = slot.model_batch;
        std::vector<uint32_t>& pib = model_batch->pib_sub_buffer;
        uint32_t last_index = safe_u32(pib.size()) - 1;

        if (slot.slot_index != last_index) {
            Entity moved_entity = pib[last_index];
            pib[slot.slot_index] = moved_entity;
            // fix the moved entity's cached slot: {model_batch, last_index} -> slot_index
            for (PibSlot& moved_slot : entity_slots[moved_entity]) {
                if (moved_slot.model_batch == model_batch && moved_slot.slot_index == last_index) {
                    moved_slot.slot_index = slot.slot_index;
                    break;
                }
            }
        }
        pib.pop_back();
        model_batch->instanceCount--;
    }
    entity_slots.erase(it);
}

void BatchBuilder::UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene)
{
    if (!scene) {
        SDL_Log("UpdateRenderBatches called with null scene!");
        return;
    }

    // Either a full rebuild (scene activation) or an incremental delta — never
    // both. exchange(false) consumes the rebuild request atomically.
    bool changed;
    if (dirty_batches.exchange(false)) {
        BuildRenderBatches(pm, pass_manager, om, scene);
        changed = true;
    }
    else {
        changed = ApplyIncremental(pm, pass_manager, om, scene);
    }

    if (changed) {
        FinalizeOffsets(pass_manager);
        ++batches_revision;
    }
}

// Assigns render_instance_base on every renderable archetype (prefix sum of entity counts).
// Must be called after any structural change to keep Entity->row mapping consistent.
inline void RecalculateInstanceOffsets(SceneData* scene)
{
    // Должно отбирать ТЕ ЖЕ архетипы и в том же порядке, что TransformDataModule
    // (инвариант «строка трансформа = render_instance_base + индекс в архетипе»).
    uint32_t base = 0;
    for (auto& [sig, arch] : scene->archetypes) {
        if (arch.get_array<DrawComponent>() &&
            arch.get_array<Positions>()) {
            arch.render_instance_base = base;
            base += safe_u32(arch.entities.size());
        }
    }
}

void BatchBuilder::BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene)
{
    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses()) {
        rp->shader_batches.clear();
    }
    entity_slots.clear();

    // A full rebuild reflects the current ECS state, which already accounts for
    // any queued create/delete. Discard the delta under the lock so it does not
    // leak into the next incremental pass.
    {
        std::lock_guard<std::mutex> lock(delta_mutex);
        entities_to_create.clear();
        entities_to_delete.clear();
    }

    // Отбор по маркеру DrawComponent (+Positions для матрицы). Геометрия/материал
    // не часть сигнатуры — тянем через Has/GetComponent. Голый рисуемый энтити без
    // модели/материала получит лишь трансформ-строку и не батчится.
    om->ForEach<DrawComponent, Positions>(
        scene,
        [&](Entity entity, const DrawComponent& draw, const Positions&)
    {
        if (!draw.visible) return;  // скрытые (выключенные в UI debug-рамки) в дерево не идут
        if (!om->Has<ModelComponent>(scene, entity) || !om->Has<MaterialComponent>(scene, entity))
            return;
        const MaterialComponent& material_component = om->GetComponent<MaterialComponent>(scene, entity);
        const ModelComponent& model_component = om->GetComponent<ModelComponent>(scene, entity);
        AddEntityToBatches(entity, pm, material_component, model_component);
    }
    );

    RecalculateInstanceOffsets(scene);
}

bool BatchBuilder::ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene)
{
    // Atomically take + clear the queues, then work on the local copies outside
    // the lock so we never hold delta_mutex while mutating the batch tree.
    std::vector<Entity> creates, deletes;
    {
        std::lock_guard<std::mutex> lock(delta_mutex);
        creates.swap(entities_to_create);
        deletes.swap(entities_to_delete);
    }
    if (creates.empty() && deletes.empty()) return false;

    // An entity created AND deleted in the same frame is dropped from the add
    // side (its components are already gone from ECS). RemoveEntityFromBatches is
    // a no-op for it, so it never reaches the batch tree.
    std::unordered_set<Entity> deleted_set(deletes.begin(), deletes.end());

    for (Entity entity : creates) {
        if (deleted_set.count(entity)) continue;
        // Идемпотентность: видимость тыкают повторно (в отличие от одноразового
        // создания энтити). Если энтити уже в дереве — повторный AddEntityToBatches
        // наплодил бы дубликаты слотов в PIB. «Show» уже видимого — просто no-op.
        if (entity_slots.count(entity)) continue;
        // Батчим только если есть и модель, и материал (см. BuildRenderBatches).
        if (!om->Has<ModelComponent>(scene, entity) || !om->Has<MaterialComponent>(scene, entity))
            continue;
        // Скрытый энтити в дерево не добавляем — флаг visible источник истины (тот же
        // отбор, что в BuildRenderBatches). Нет DrawComponent — тоже не рисуемый.
        if (!om->Has<DrawComponent>(scene, entity) || !om->GetComponent<DrawComponent>(scene, entity).visible)
            continue;
        const MaterialComponent& material_component = om->GetComponent<MaterialComponent>(scene, entity);
        const ModelComponent& model_component = om->GetComponent<ModelComponent>(scene, entity);
        AddEntityToBatches(entity, pm, material_component, model_component);
    }
    for (Entity entity : deletes) {
        RemoveEntityFromBatches(entity);
    }

    RecalculateInstanceOffsets(scene);
    return true;
}

void BatchBuilder::FinalizeOffsets(PassManager* pass_manager)
{
    uint32_t offset = 0;
    uint32_t command_index = 0;

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses())
    {
        for (auto& [shader_key, shader_batch] : rp->shader_batches)
        {
            for (auto& [atlas_key, atlas_batch] : shader_batch.atlases_batches)
            {
                for (auto& [texture_key, texture_batch] : atlas_batch.texture_batches)
                {
                    texture_batch.indirect_command_index = command_index;

                    for (auto& [model_key, model_batch] : texture_batch.model_batches)
                    {
                        model_batch.firstInstance = offset;
                        offset += model_batch.instanceCount;
                        command_index++;
                    }
                }
            }
        }
    }
    total_commands = command_index;
}

void BatchBuilder::BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm) {
    for (auto& rp : pass_manager->GetOrderedComputePasses()) {
        rp->shader_batches.clear();
    }
    for (auto& rp : pass_manager->GetOrderedComputePrepasses()) {
        rp->shader_batches.clear();
    }
    if (!sm || !sm->IsDirtyComputePipelines()) return;

    auto& compute_programs = sm->GetComputeShaderPrograms();
    for (auto& sp : compute_programs) {
        SDL_GPUComputePipeline* pipe = pm->GetComputePipeline(sp.get());
        if (!pipe) continue;

        ComputePassStep* cmp = sp->associated_compute_pass;
        if (!cmp) continue;

        ComputeShaderBatchData new_batch{};
        new_batch.pipeline = pipe;
        new_batch.rw_storage_buffers = sp->rw_storage_buffers;
        new_batch.ro_storage_buffers = sp->ro_storage_buffers;

        new_batch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            new_batch.rw_storage_textures.emplace_back(
                d.texture_atlas->texture_binding.texture, d.mip_level, d.layer, false);
        }
        new_batch.ro_storage_textures.reserve(sp->ro_storage_textures.size());
        for (const auto& a : sp->ro_storage_textures) {
            new_batch.ro_storage_textures.push_back(a->texture_binding.texture);
        }
        new_batch.texture_binding.reserve(sp->texture_samplers.size());
        for (const auto& a : sp->texture_samplers) {
            new_batch.texture_binding.push_back(a->texture_binding);
        }
        new_batch.push_func = sp->push_func;
        new_batch.dispatch_func = sp->dispatch_func;

        new_batch.threadcount_x = sp->cs.threadcount_x;
        new_batch.threadcount_y = sp->cs.threadcount_y;
        new_batch.threadcount_z = sp->cs.threadcount_z;

        new_batch.debug_name = sp->debug_name;

        cmp->shader_batches.push_back(std::move(new_batch));
    }
    sm->SetDirtyComputePipelines(false);
}



uint32_t BatchBuilder::AskNumCommands()
{
    return total_commands;
}
