#include "PCH.h"
#include "BatchBuilder.h"
#include "RenderCommandData.h"
#include "RenderSnapshot.h"   // BatchLayout — слепок раскладки, который строит FinalizeOffsets
#include "ObjectManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"   // резолв имён текстур материала → TextureHandle на сборке батча
#include "BufferManager.h"    // резолв имён storage-буферов sp → BufferData* на сборке батча
#include "ModelData.h"
#include "TextureData.h"
#include <unordered_set>

using namespace BatchKeys;    // ключи батчей — локально для TU (в заголовках квалифицированы)
using namespace ShaderBase;   // вершинные типы/семантики


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

AtlasBatchKey HashAtlasBatchKey(const Material* mat, TextureManager* tm) {
    if (!mat) {
        SDL_Log("HashAtlasBatchKey: material is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    AtlasBatchKey key = 0;
    for (const auto& [slot, tex_name] : mat->textures) {
        // имя → хэндл (name-based ссылка); промах/удалённая → в ключ не входит (в батче будет dummy)
        TextureHandle* h = tm ? tm->GetTextureHandle(tex_name) : nullptr;
        if (h && h->atlas) {
            key ^= reinterpret_cast<AtlasBatchKey>(h->atlas) * (2654435761ull + static_cast<uint64_t>(slot));
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

void BatchBuilder::AddEntityToBatches(Entity entity, PipeManager* pm, TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    const MaterialComponent& material_component, const ModelComponent& model_component) {

    // Защита от незаполненных ссылок: сущность из загрузки сцены, чьё имя ассета не
    // разрешилось в указатель (пустое/неизвестное имя), приходит с model == nullptr.
    // Без этого гарда разыменование model->submeshes падает в сборке батчей.
    if (!model_component.model) {
        SDL_Log("BatchBuilder: entity %u has null model — skipped (unresolved asset?)", entity);
        return;
    }

    for (SubMeshData& submesh : model_component.model->submeshes)
    {
        if (material_component.materials.size() != model_component.model->submeshes.size()) {
            SDL_Log("BulidBatches:: Submash and material sizes mismatch");
        }
        // Границы + null материала (тоже может быть не разрешён по имени при загрузке).
        if (submesh.material_index >= material_component.materials.size()) {
            SDL_Log("BatchBuilder: entity %u material_index out of range — skipped", entity);
            continue;
        }
        Material* material = material_component.materials[submesh.material_index];
        if (!material) {
            SDL_Log("BatchBuilder: entity %u has null material — skipped (unresolved asset?)", entity);
            continue;
        }

        for (const ShaderName& sp_name : material->shader_programs)
        {
            // name-based ссылка: имя sp → указатель на сборке батча. Промах (sp удалена) → fallback-sp
            // (аналог textureless), а если и его нет — пропуск.
            ShaderProgram* sp = sm ? sm->GetShaderProgram(sp_name) : nullptr;
            if (!sp) {
                // sp удалена → fallback ПО ИМЕНИ (резолвим как обычную sp; удалён и он → пустой рендер, без краша).
                SDL_Log("BatchBuilder::Material references deleted shader program '%s' — using fallback", sp_name.c_str());
                sp = (sm && !fallback_shader_name.empty()) ? sm->GetShaderProgram(fallback_shader_name) : nullptr;
                if (!sp) continue;
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
                // Буферы sp — по имени (BufferDataName = ключ реестра); резолвим в BufferData* здесь
                // (как имена текстур/sp выше) через GetBufferData. Ненайденное имя пропускаем — слот
                // бинда сдвинется, но висячего указателя не будет.
                auto resolve_buffers = [bm](const std::vector<BufferDataName>& names) {
                    std::vector<BufferData*> out; out.reserve(names.size());
                    for (BufferDataName n : names)
                        if (BufferData* b = bm->GetBufferData(n)) out.push_back(b);
                    return out;
                };
                new_batch.vertexStorageBuffers   = resolve_buffers(sp->vertex_shader_buffer_names);
                new_batch.fragmentStorageBuffers = resolve_buffers(sp->fragment_shader_buffer_names);
                FragmentShaderData* fsd = sm->GetFragmentShader(sp->fs_name);   // fs по имени из реестра
                new_batch.frag_uniform_count = fsd ? fsd->shader_data.num_uniform_buffers : 0u;
                shader_map[sp_key] = std::move(new_batch);
            }

            ShaderBatchData& sb = shader_map[sp_key];

            AtlasBatchKey atlas_key = sp->required_slots.empty() ? 0 : HashAtlasBatchKey(material, tm);

            // Dummy — ПО ИМЕНИ (как fallback-sp): резолв здесь, через карту (без лог-спама Get*).
            // Нет и dummy (удалён) → битые слоты не забиндить → sp у этого материала пропускается
            // (пустой рендер вместо разыменования мёртвого хэндла).
            TextureHandle* dummy = nullptr;
            if (tm && !dummy_texture_name.empty()) {
                auto dit = tm->GetTextureHandles().find(dummy_texture_name);
                if (dit != tm->GetTextureHandles().end()) dummy = dit->second.get();
            }

            auto& atlas_map = sb.atlases_batches;
            auto atlas_it = atlas_map.find(atlas_key);
            if (atlas_it == atlas_map.end())
            {
                AtlasBatchData new_tex{};
                bool bindable = true;

                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    TextureHandle* h = (it != material->textures.end() && tm)
                        ? tm->GetTextureHandle(it->second) : nullptr;
                    if (!h || !h->atlas) {   // нет слота / имя не резолвится (удалена/переименована) / нет атласа
                        SDL_Log("BuildBatches:: Material is missing required atlas for shader slot");
                        h = (dummy && dummy->atlas) ? dummy : nullptr;
                    }
                    if (!h) { bindable = false; break; }   // и dummy нет → слот не собрать
                    new_tex.texture_binding.push_back(h->atlas->texture_binding);
                }
                if (!bindable) {
                    SDL_Log("BuildBatches:: dummy texture missing too — sp draw skipped for this material");
                    continue;   // следующий sp материала: пустой рендер, без краша
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
                // Указатель ставим всегда; РЕШЕНИЕ пушить — в RenderManager по числу fragment-
                // uniform’ов шейдера (объявлен ли слот MaterialBlock). Так shadow/depth (без
                // MaterialBlock) params не получат автоматически, без флагов и прокси по текстурам.
                new_texb.params = &material->params;
                new_texb.texture_uvl.reserve(material->textures.size());
                bool bindable = true;
                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    TextureHandle* texture_handle = (it != material->textures.end() && tm)
                        ? tm->GetTextureHandle(it->second) : nullptr;
                    if (!texture_handle) {   // нет слота / имя не резолвится (удалена/переименована)
                        SDL_Log("BuildBatches:: Material is missing required texture_data for shader slot");
                        texture_handle = dummy;   // dummy по имени (см. выше); тоже нет → пропуск sp
                    }
                    if (!texture_handle) { bindable = false; break; }
                    new_texb.texture_uvl.push_back(texture_handle->texture_data);   // КОПИЯ значения (см. инвариант в TextureBatchData)
                }
                if (!bindable) {
                    SDL_Log("BuildBatches:: dummy texture missing too — sp draw skipped for this material");
                    continue;   // пустой рендер, без краша
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

void BatchBuilder::UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
    TextureManager* tm, ShaderManager* sm, BufferManager* bm, SceneData* scene)
{
    if (!scene) {
        SDL_Log("UpdateRenderBatches called with null scene!");
        return;
    }

    // Either a full rebuild (scene activation) or an incremental delta — never
    // both. exchange(false) consumes the rebuild request atomically.
    // Замок дерева больше не нужен: рендер живое дерево не читает (рисует по слепку
    // раскладки слота — см. FinalizeOffsets/AskLayout), дерево приватно для sim.
    bool changed = false;
    if (dirty_batches.exchange(false)) {
        BuildRenderBatches(pm, pass_manager, om, tm, sm, bm, scene);
        FinalizeOffsets(pass_manager);
        // Полная пересборка переклеила ВСЮ раскладку (indirect_command_index, firstInstance).
        // Бампим эпоху — слоты, залитые под старой раскладкой, рендер больше не покажет.
        ++rebuild_epoch;
        changed = true;
    }
    else if (ApplyIncremental(pm, pass_manager, om, tm, sm, bm, scene)) {
        FinalizeOffsets(pass_manager);
        changed = true;
    }

    if (changed) {
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

void BatchBuilder::BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
    TextureManager* tm, ShaderManager* sm, BufferManager* bm, SceneData* scene)
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
        AddEntityToBatches(entity, pm, tm, sm, bm, material_component, model_component);
    }
    );

    RecalculateInstanceOffsets(scene);
}

bool BatchBuilder::ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
    TextureManager* tm, ShaderManager* sm, BufferManager* bm, SceneData* scene)
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
        AddEntityToBatches(entity, pm, tm, sm, bm, material_component, model_component);
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

    // Одним обходом: проставляем офсеты в ДЕРЕВЕ (его читают sim-модули — indirect/PIB
    // при заливке буферов) и строим НОВУЮ версию СЛЕПКА раскладки (RenderSnap::BatchLayout) —
    // значения-двойник дерева для рендера. Слоты получают её в StampLayoutSnapshot: рендер
    // слота k рисует ровно по раскладке, под которую залит его indirect_buffer[k].
    auto layout = std::make_shared<RenderSnap::BatchLayout>();
    layout->passes.reserve(pass_manager->GetOrderedRenderPasses().size());

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses())
    {
        RenderSnap::PassDrawList pass_list;
        pass_list.first_instance = offset;
        pass_list.shaders.reserve(rp->shader_batches.size());

        for (auto& [shader_key, shader_batch] : rp->shader_batches)
        {
            RenderSnap::ShaderGroup sg;
            sg.pipeline = shader_batch.pipeline;
            sg.push_func = shader_batch.push_func;
            sg.vertexStorageBuffers = shader_batch.vertexStorageBuffers;
            sg.fragmentStorageBuffers = shader_batch.fragmentStorageBuffers;
            sg.frag_uniform_count = shader_batch.frag_uniform_count;
            sg.atlases.reserve(shader_batch.atlases_batches.size());

            for (auto& [atlas_key, atlas_batch] : shader_batch.atlases_batches)
            {
                RenderSnap::AtlasGroup ag;
                ag.texture_binding = atlas_batch.texture_binding;
                ag.draws.reserve(atlas_batch.texture_batches.size());

                for (auto& [texture_key, texture_batch] : atlas_batch.texture_batches)
                {
                    texture_batch.indirect_command_index = command_index;

                    RenderSnap::TextureDraw td;
                    td.texture_uvl = texture_batch.texture_uvl;
                    td.params = texture_batch.params;   // невладеющий, адрес стабилен (см. RenderSnapshot.h)
                    td.indirect_command_index = command_index;
                    td.draw_count = safe_u32(texture_batch.model_batches.size());

                    for (auto& [model_key, model_batch] : texture_batch.model_batches)
                    {
                        model_batch.firstInstance = offset;
                        offset += model_batch.instanceCount;
                        command_index++;
                    }
                    ag.draws.push_back(std::move(td));
                }
                sg.atlases.push_back(std::move(ag));
            }
            pass_list.shaders.push_back(std::move(sg));
        }
        // Сумма инстансов прохода: у SHADOW_PASS (первый по pass_index) это shadow_pib —
        // граница PIB между теневыми [0, sp) и остальными [sp, N) записями для каллинга.
        pass_list.num_instances = offset - pass_list.first_instance;
        layout->passes.push_back(std::move(pass_list));
    }
    layout->num_commands = command_index;
    layout->num_instances = offset;   // сумма инстансов всех батчей = число PIB-записей
    current_layout = std::move(layout);
}

// Слепок раскладки слоту — O(1). Зовётся в PrepareFunc СРАЗУ после UpdateRenderBatches,
// до заливки буферов слота: всё, что prepare зальёт (indirect/PIB/out_pib), соответствует
// именно этой версии раскладки.
void BatchBuilder::StampLayoutSnapshot(uint8_t slot)
{
    slot_layouts[slot] = current_layout;
}

uint32_t BatchBuilder::AskNumCommands(uint8_t slot) const
{
    const RenderSnap::BatchLayout* l = slot_layouts[slot].get();
    return l ? l->num_commands : 0u;
}

uint32_t BatchBuilder::AskNumInstances(uint8_t slot) const
{
    const RenderSnap::BatchLayout* l = slot_layouts[slot].get();
    return l ? l->num_instances : 0u;
}

void BatchBuilder::BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm) {
    // Батчи ПЕРСИСТЕНТНЫ: пересобираем только при создании compute-программ (флаг), не каждый кадр.
    // Батч хранит СТАБИЛЬНЫЕ TextureAtlas* (не снапшот SDL_GPUTexture*), а резолв в актуальные
    // биндинги — в ComputePassStandardBody. Поэтому ресайз (пересоздание текстур) ребилда НЕ требует.
    if (!sm || !sm->IsDirtyComputeBatches()) return;

    // Без замка: пересборка случается только при СОЗДАНИИ compute-программ, а все программы
    // создаются на инициализации, до старта потоков — параллельного рендера ещё нет.
    // (Если программы когда-то начнут создаваться в рантайме — список нужно будет отдавать
    // версией через shared_ptr, как BatchLayout.)
    for (auto& rp : pass_manager->GetOrderedComputePasses()) {
        rp->shader_batches.clear();
    }
    for (auto& rp : pass_manager->GetOrderedComputePrepasses()) {
        rp->shader_batches.clear();
    }

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

        // Копируем СТАБИЛЬНЫЕ атласы (без резолва SDL_GPUTexture* — он в ComputePassStandardBody).
        new_batch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            new_batch.rw_storage_textures.push_back({ d.texture_atlas, d.mip_level, d.layer });
        }
        new_batch.ro_storage_textures = sp->ro_storage_textures;   // vector<TextureAtlas*>
        new_batch.texture_binding     = sp->texture_samplers;      // vector<TextureAtlas*>, даёт texture+sampler
        new_batch.push_func = sp->push_func;
        new_batch.dispatch_func = sp->dispatch_func;

        ComputeShaderData* csd = sm->GetComputeShader(sp->cs_name);   // cs по имени из реестра
        new_batch.threadcount_x = csd ? csd->threadcount_x : 1u;
        new_batch.threadcount_y = csd ? csd->threadcount_y : 1u;
        new_batch.threadcount_z = csd ? csd->threadcount_z : 1u;

        new_batch.debug_name = sp->debug_name;

        cmp->shader_batches.push_back(std::move(new_batch));
    }
    sm->SetDirtyComputeBatches(false);
    // Дерево пересобрано — старых указателей пайплайнов в нём больше нет. Бамп армирует
    // отложенное удаление compute-пайплайнов (TrashPipelines дренирует in-flight и освобождает).
    compute_rebuild_epoch.fetch_add(1, std::memory_order_release);
}
