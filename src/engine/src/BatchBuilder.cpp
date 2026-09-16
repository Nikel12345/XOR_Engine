#include "PCH.h"
#include "BatchBuilder.h"
#include "BaseComponents.h"
#include "RenderCommandData.h"
#include "RenderSnapshot.h"
#include "ObjectManager.h"
#include "PipeManager.h"
#include "PassManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "BufferManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "PositionStructure.h"
#include "ModelData.h"
#include "TextureData.h"
#include <unordered_set>

using namespace BatchKeys;
using namespace ShaderBase;


ModelBatchKey HashModelBatchKey(const std::string& model_name, uint32_t submesh_index) {
    ModelBatchKey key = std::hash<std::string>{}(model_name);
    key ^= static_cast<ModelBatchKey>(submesh_index) + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);

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

// «Параметров нет»: общий пустой shared_ptr, чтобы у тернарника выше был один тип.
const std::shared_ptr<std::vector<uint8_t>> kNoParams{};

// Ключ ПАМЯТКИ предпрохода, а не узла дерева: две ячейки одного материала могут отрезолвиться
// в одну sp (обе упали на фолбэк), а блобы у них разные.
MatSpKey HashMatSpMemo(const Material* mat, const ShaderProgram* sp,
                       const std::vector<uint8_t>* params) {
    MatSpKey key = reinterpret_cast<MatSpKey>(mat);
    key = MixKey(key ^ reinterpret_cast<MatSpKey>(sp));
    return MixKey(key ^ reinterpret_cast<MatSpKey>(params));
}

MatSpKey HashMatSpResources(const ShaderProgram* sp, const std::vector<uint8_t>* params,
                            const SlotWord* slot_words,
                            const std::vector<const TextureHandle*>& block_handles) {
    if (!sp) {
        return 0xFFFFFFFFFFFFFFFFull;
    }
    MatSpKey key = 0;
    const size_t slot_count = std::min<size_t>(sp->required_slots.size(), MAX_SLOTS);
    for (size_t s = 0; s < slot_count; ++s) {
        // Роль и её адресация идут в ключ всегда: «текстуры нет» — такое же состояние узла.
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
    return MixKey(key);
}

TextureBatchKey HashTextureBatchKey(MatSpKey res_key, uint32_t material_index) {
    return MixKey(res_key ^ (static_cast<TextureBatchKey>(material_index) + 0x9e3779b97f4a7c15ull));
}

AtlasBatchKey HashAtlasBatchKey(const ShaderProgram* sp,
                                const std::vector<const TextureHandle*>& block_handles) {
    if (!sp) {
        return 0xFFFFFFFFFFFFFFFFull;
    }
    AtlasBatchKey key = 0;
    for (TextureSlotRole slot : sp->required_slots) {
        key += static_cast<AtlasBatchKey>(slot) + 0x9e3779b97f4a7c15ull;
        key *= 0xff51afd7ed558ccd;
        key ^= key >> 29;
    }
    for (const TextureHandle* h : block_handles) {
        key ^= reinterpret_cast<AtlasBatchKey>(h ? h->atlas : nullptr);
        key *= 0xff51afd7ed558ccd;
        key ^= key >> 29;
    }
    return MixKey(key);
}

ShaderBatchKey HashShaderBatchKey(ShaderProgram* sp) {
    if (!sp) {
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

void BatchBuilder::SetDummyTexture(const std::string& name, TextureManager* tm)
{
    dummy_texture_name = name;
    if (!tm) return;
    const auto& handles = tm->GetTextureHandles();
    auto it = handles.find(name);
    if (it == handles.end() || !it->second) return;
    TextureAtlas* atlas = it->second->atlas;
    if (!atlas) return;

    if (atlas->texture_binding.texture && !(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "USAGE VIOLATION: dummy texture '%s' lives in atlas '%s', whose GPU texture was ALREADY "
            "CREATED without SDL_GPU_TEXTUREUSAGE_SAMPLER. It IS bound as a fragment sampler "
            "(fallback for missing material slots) - the bind will abort. "
            "Declare SAMPLER at atlas creation.",
            name.c_str(), atlas->debug_name.c_str());
    }

    // Своего материала у dummy нет, поэтому SAMPLER его атласу объявляем здесь.
    atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
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

void BatchBuilder::QueueUpdate(Entity entity)
{
    std::lock_guard<std::mutex> lock(delta_mutex);
    entities_to_update.push_back(entity);
}

void BatchBuilder::BuildMaterialLayouts(TextureManager* tm, ShaderManager* sm, MaterialManager* mtm)
{
    mat_sp_layouts.clear();
    if (!mtm || !sm) return;

    TextureHandle* dummy = nullptr;
    if (tm && !dummy_texture_name.empty()) {
        auto dit = tm->GetTextureHandles().find(dummy_texture_name);
        if (dit != tm->GetTextureHandles().end()) dummy = dit->second.get();
    }
    if (dummy && !dummy->atlas) dummy = nullptr;
    ShaderProgram* fallback = fallback_shader_name.empty() ? nullptr : sm->GetShaderProgram(fallback_shader_name);

    std::vector<const TextureHandle*> block_handles;

    for (const auto& [mat_name, mat_owner] : mtm->GetMaterials()) {
        Material* material = mat_owner.get();
        if (!material) continue;

        // Порядок ячеек — одно определение на движок: его же читает TextureStateDataModule.
        const VariativeRoles cells = CollectVariativeRoles(*material);

        for (const SpBinding& binding : material->shader_programs) {
            const std::shared_ptr<std::vector<uint8_t>>& sp_params =
                (binding.params && !binding.params->empty()) ? binding.params : kNoParams;
            ShaderProgram* sp = sm->GetShaderProgram(binding.sp);
            if (!sp) sp = fallback;
            if (!sp) continue;

            const MatSpKey memo = HashMatSpMemo(material, sp, sp_params.get());
            if (mat_sp_layouts.count(memo)) continue;

            MatSpLayout lay{};
            lay.bindable = true;
            block_handles.clear();
            lay.uvl.reserve(sp->required_slots.size());
            lay.texture_binding.reserve(sp->required_slots.size());

            for (size_t s = 0; s < sp->required_slots.size(); ++s) {
                const TextureSlotRole role = sp->required_slots[s];
                auto it = material->textures.find(role);
                const std::vector<TextureName>* names =
                    (it != material->textures.end()) ? &it->second : nullptr;

                const uint32_t base = safe_u32(lay.uvl.size());

                TextureHandle* def = (names && !names->empty() && tm)
                    ? tm->GetTextureHandle((*names)[0]) : nullptr;
                // Место дефолта в таблице сохраняется всегда: на нём стоит base следующих слотов.
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
                        // Неразрешимый вариант в таблицу не попадает, и счётчик вариантов не
                        // растёт: иначе base последующих слотов разъехался бы с реальной таблицей.
                        if (!h || !h->atlas) continue;
                        lay.uvl.push_back(MakeUVL(h->texture_data));
                        block_handles.push_back(h);
                        ++count;
                    }
                }
                if (count == 1) cell = 0;
                else            lay.variative = true;

                // Материал без вариантов обязан давать прежнюю плотную таблицу: base[s] == s.
                assert((cells.count != 0 || (count == 1 && base == safe_u32(s)))
                    && "BuildMaterialLayouts: material without variants must yield the legacy UVL table");

                if (s < MAX_SLOTS)
                    lay.slot[s] = { safe_u32_u8(count), safe_u32_u8(cell), safe_u32_u16(base) };
            }

            if (!lay.bindable) {
                lay.uvl.clear();
                lay.texture_binding.clear();
            }
            else {
                lay.res_key   = HashMatSpResources(sp, sp_params.get(), lay.slot, block_handles);
                lay.atlas_key = sp->required_slots.empty() ? 0 : HashAtlasBatchKey(sp, block_handles);
            }
            mat_sp_layouts.emplace(memo, std::move(lay));
        }
    }
}

void BatchBuilder::AddEntityToBatches(Entity entity, PipeManager* pm, PassManager* pass_manager, TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm,
    const MaterialComponent& material_component, const ModelComponent& model_component) {

    // Резолв ТИХИЙ (FindModel, а не логирующий operator[]): он идёт на КАЖДУЮ сущность, и одно
    // битое имя в сцене на миллион объектов дало бы миллион строк лога.
    ModelData* model = mdm ? mdm->FindModel(model_component.name) : nullptr;

    if (!model) {
        return;
    }

    uint32_t submesh_index = 0;
    for (SubMeshData& submesh : model->submeshes)
    {
        const uint32_t si = submesh_index++;
        if (submesh.indexCount == 0) continue;

        if (submesh.material_index >= material_component.materials.size()) {
            continue;
        }
        const std::string& material_name = material_component.materials[submesh.material_index].name;
        Material* material = nullptr;
        if (mtm && !material_name.empty()) {
            const auto& materials = mtm->GetMaterials();
            auto mit = materials.find(material_name);
            if (mit != materials.end()) material = mit->second.get();
        }
        if (!material) {
            continue;
        }

        for (const SpBinding& binding : material->shader_programs)
        {
            const ShaderName& sp_name = binding.sp;
            const std::shared_ptr<std::vector<uint8_t>>& sp_params =
                (binding.params && !binding.params->empty()) ? binding.params : kNoParams;
            ShaderProgram* sp = sm ? sm->GetShaderProgram(sp_name) : nullptr;
            // Имя РЕАЛЬНО взятой программы: по нему резолвятся push-инструкции, и на фолбэк-ветке
            // с запрошенным именем программа получила бы чужие пуши.
            const ShaderName* resolved_name = &sp_name;
            if (!sp) {
                sp = (sm && !fallback_shader_name.empty()) ? sm->GetShaderProgram(fallback_shader_name) : nullptr;
                if (!sp) continue;
                resolved_name = &fallback_shader_name;
            }
            RenderPassStep* rp = pass_manager->GetRenderPassStep(sp->render_pass_name);
            if (!rp) continue;

            auto& shader_map = rp->shader_batches;
            auto sp_key = HashShaderBatchKey(sp);
            auto it = shader_map.find(sp_key);
            if (it == shader_map.end())
            {
                auto pipe = pm->GetGraphicPipeline(sp);
                if (!pipe) continue;

                ShaderBatchData new_batch{};
                new_batch.push_instructions = sm->CollectPushInstructions(*resolved_name);
                new_batch.pipeline = std::move(pipe);
                auto resolve_buffers = [bm](const std::vector<BufferDataName>& names) {
                    std::vector<BufferData*> out; out.reserve(names.size());
                    for (BufferDataName n : names)
                        if (BufferData* b = bm->GetBufferData(n)) out.push_back(b);
                    return out;
                };
                new_batch.vertexStorageBuffers   = resolve_buffers(sp->vertex_shader_buffer_names);
                new_batch.fragmentStorageBuffers = resolve_buffers(sp->fragment_shader_buffer_names);

                if (VertexShaderData* vsd = sm->GetVertexShader(sp->vs_name)) {
                    new_batch.vertexBuffers = resolve_buffers(vsd->vertex_buffer_names);
                    if (vsd->index_buffer)
                        new_batch.indexBuffer = bm->GetBufferData(vsd->index_buffer);
                }
                shader_map[sp_key] = std::move(new_batch);
            }

            ShaderBatchData& sb = shader_map[sp_key];

            auto lay_it = mat_sp_layouts.find(HashMatSpMemo(material, sp, sp_params.get()));
            if (lay_it == mat_sp_layouts.end()) continue;
            const MatSpLayout& lay = lay_it->second;
            if (!lay.bindable) continue;

            auto& atlas_map = sb.atlases_batches;
            auto atlas_it = atlas_map.find(lay.atlas_key);
            if (atlas_it == atlas_map.end())
            {
                AtlasBatchData new_tex{};
                new_tex.texture_binding = lay.texture_binding;
                atlas_map[lay.atlas_key] = std::move(new_tex);
            }

            AtlasBatchData& atlas_batch = atlas_map[lay.atlas_key];

            // Номер материала нужен только узлу с вариантами: иначе он дробил бы узел по номеру
            // сабмеша, ничего не меняя в пуше.
            const uint32_t material_index = lay.variative ? submesh.material_index : 0u;
            TextureBatchKey tex_key = HashTextureBatchKey(lay.res_key, material_index);

            auto& tex_map = atlas_batch.texture_batches;
            auto texb_it = tex_map.find(tex_key);
            if (texb_it == tex_map.end()) {
                TextureBatchData new_texb{};
                new_texb.params = sp_params;
                new_texb.texture_uvl = lay.uvl;
                std::copy(std::begin(lay.slot), std::end(lay.slot), std::begin(new_texb.variant_layout.slot));
                new_texb.variant_layout.material_index = material_index;

                tex_map[tex_key] = std::move(new_texb);
            }

            TextureBatchData& tex_batch = tex_map[tex_key];
            ModelBatchKey model_key = HashModelBatchKey(model_component.name, si);

            auto& model_map = tex_batch.model_batches;
            auto model_it = model_map.find(model_key);
            if (model_it == model_map.end())
            {
                ModelBatchData new_model{};
                new_model.submesh = { submesh.indexCount, submesh.indexOffset,
                                      submesh.vertexOffset, submesh.screen_size_span };
                new_model.instanceCount = 0;
                new_model.pib_sub_buffer.reserve(16);
                model_map[model_key] = std::move(new_model);
            }

            ModelBatchData& model_batch = model_map[model_key];

            uint32_t slot_index = safe_u32(model_batch.pib_sub_buffer.size());
            model_batch.instanceCount++;
            // Строка ещё не известна: базы архетипов раздаёт RecalculateInstanceOffsets в конце
            // сборки, а саму строку добьёт ближайшая заливка PIB.
            model_batch.pib_sub_buffer.push_back({ entity, kPibNoRow });
            entity_slots[entity].push_back({ &model_batch, slot_index });

        }
    }

}

void BatchBuilder::RemoveEntityFromBatches(Entity entity)
{
    auto it = entity_slots.find(entity);
    if (it == entity_slots.end()) return;

    for (const PibSlot& slot : it->second) {
        ModelBatchData* model_batch = slot.model_batch;
        std::vector<PibRecord>& pib = model_batch->pib_sub_buffer;
        uint32_t last_index = safe_u32(pib.size()) - 1;

        if (slot.slot_index != last_index) {
            Entity moved_entity = pib[last_index].entity;
            pib[slot.slot_index] = pib[last_index];
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
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, SceneData* scene)
{
    if (!scene) return;

    // Предпроход ДО развилки: AddEntityToBatches общая для полной пересборки и инкремента, и обе
    // стороны читают памятку.
    BuildMaterialLayouts(tm, sm, mtm);

    bool changed = false;
    if (dirty_batches.exchange(false)) {
        BuildRenderBatches(pm, pass_manager, om, tm, sm, bm, mdm, mtm, scene);
        FinalizeOffsets(pass_manager, bm);
        ++rebuild_epoch;
        changed = true;
    }
    else if (ApplyIncremental(pm, pass_manager, om, tm, sm, bm, mdm, mtm, scene)) {
        FinalizeOffsets(pass_manager, bm);
        changed = true;
    }

    if (changed) {
        ++batches_revision;
    }
}

// Отбор архетипов и их порядок обязаны совпадать с TransformDataModule: строка трансформа =
// база архетипа + индекс сущности в нём.
inline void RecalculateInstanceOffsets(SceneData* scene)
{
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
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, SceneData* scene)
{
    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses()) {
        rp->shader_batches.clear();
    }
    entity_slots.clear();

    {
        std::lock_guard<std::mutex> lock(delta_mutex);

        entities_to_create.clear();
        entities_to_delete.clear();
        entities_to_update.clear();
    }

    // Отбор по маркеру DrawComponent, Positions НЕ требуется: transformless-дровабл (скайбокс
    // строит позицию из камеры) батчится как все, просто строки у него нет.
    om->ForEach<DrawComponent>(
        scene,
        [&](Entity entity, const DrawComponent& draw)
    {
        if (!draw.visible) return;
        if (!om->Has<ModelComponent>(scene, entity) || !om->Has<MaterialComponent>(scene, entity))
            return;
        const MaterialComponent& material_component = om->GetComponent<MaterialComponent>(scene, entity);
        const ModelComponent& model_component = om->GetComponent<ModelComponent>(scene, entity);
        AddEntityToBatches(entity, pm, pass_manager, tm, sm, bm, mdm, mtm, material_component, model_component);
    }
    );

    RecalculateInstanceOffsets(scene);
}

bool BatchBuilder::ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, SceneData* scene)
{
    std::vector<Entity> creates, deletes, updates;
    {
        std::lock_guard<std::mutex> lock(delta_mutex);
        creates.swap(entities_to_create);
        deletes.swap(entities_to_delete);
        updates.swap(entities_to_update);
    }
    if (creates.empty() && deletes.empty() && updates.empty()) return false;

    auto add_if_drawable = [&](Entity entity) {
        if (!om->Has<ModelComponent>(scene, entity) || !om->Has<MaterialComponent>(scene, entity))
            return;
        if (!om->Has<DrawComponent>(scene, entity) || !om->GetComponent<DrawComponent>(scene, entity).visible)
            return;
        const MaterialComponent& material_component = om->GetComponent<MaterialComponent>(scene, entity);
        const ModelComponent& model_component = om->GetComponent<ModelComponent>(scene, entity);
        AddEntityToBatches(entity, pm, pass_manager, tm, sm, bm, mdm, mtm, material_component, model_component);
    };

    // «Перевесить» — первыми: после этого энтити уже в дереве, поэтому парный QueueCreate погасит
    // гард идемпотентности, а парный QueueDelete отработает ниже и уберёт её целиком.
    for (Entity entity : updates) {
        RemoveEntityFromBatches(entity);
        add_if_drawable(entity);
    }

    // Созданная И удалённая в одном кадре с add-стороны выпадает: её компонентов в ECS уже нет.
    std::unordered_set<Entity> deleted_set(deletes.begin(), deletes.end());

    for (Entity entity : creates) {
        if (deleted_set.count(entity)) continue;
        // Видимость тыкают повторно, и повторный Add наплодил бы дубликаты слотов в PIB.
        if (entity_slots.count(entity)) continue;
        add_if_drawable(entity);
    }
    for (Entity entity : deletes) {
        RemoveEntityFromBatches(entity);
    }

    RecalculateInstanceOffsets(scene);
    return true;
}

void BatchBuilder::FinalizeOffsets(PassManager* pass_manager, BufferManager* bm)
{
    uint32_t offset = 0;

    auto layout = std::make_shared<RenderSnap::BatchLayout>();
    layout->passes.reserve(pass_manager->GetOrderedRenderPasses().size());
    layout->indirectBuffer = bm->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER);

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses())
    {
        RenderSnap::PassDrawList pass_list;
        pass_list.first_instance = offset;
        pass_list.shaders.reserve(rp->shader_batches.size());

        // Нумерация команд ЛОКАЛЬНА для прохода: его регион содержит только его команды
        // (см. docs/internals/culling.md).
        uint32_t pass_cmd_index = 0;

        uint32_t pass_cmds = 0;

        pass_list.global_texture_bindings.reserve(rp->global_texture_bindings.size());

        for (TextureAtlas* atlas : rp->global_texture_bindings) {
            if (!atlas || !atlas->texture_binding.texture) {
                SDL_Log("BatchBuilder::FinalizeOffsets missing GPU texture in pass global_texture_bindings");
                continue;
            }
            pass_list.global_texture_bindings.push_back(atlas->texture_binding);
        }

        for (auto& [shader_key, shader_batch] : rp->shader_batches)
        {
            RenderSnap::ShaderGroup sg;
            sg.pipeline = shader_batch.pipeline;
            sg.push_instructions = shader_batch.push_instructions;
            sg.vertexBuffers = shader_batch.vertexBuffers;
            sg.indexBuffer = shader_batch.indexBuffer;
            sg.vertexStorageBuffers = shader_batch.vertexStorageBuffers;
            sg.fragmentStorageBuffers = shader_batch.fragmentStorageBuffers;
            sg.atlases.reserve(shader_batch.atlases_batches.size());

            for (auto& [atlas_key, atlas_batch] : shader_batch.atlases_batches)
            {
                RenderSnap::AtlasGroup ag;
                ag.texture_binding = atlas_batch.texture_binding;
                ag.draws.reserve(atlas_batch.texture_batches.size());

                for (auto& [texture_key, texture_batch] : atlas_batch.texture_batches)
                {
                    texture_batch.indirect_command_index = pass_cmd_index;

                    RenderSnap::TextureDraw td;
                    td.texture_uvl = texture_batch.texture_uvl;
                    td.variant_layout = texture_batch.variant_layout;
                    td.params = texture_batch.params;
                    td.indirect_command_index = pass_cmd_index;
                    td.draw_count = safe_u32(texture_batch.model_batches.size());

                    for (auto& [model_key, model_batch] : texture_batch.model_batches)
                    {
                        model_batch.firstInstance = offset;
                        offset += model_batch.instanceCount;
                        pass_cmd_index++;
                    }
                    pass_cmds += td.draw_count;
                    ag.draws.push_back(std::move(td));
                }
                sg.atlases.push_back(std::move(ag));
            }
            pass_list.shaders.push_back(std::move(sg));
        }
        pass_list.num_instances = offset - pass_list.first_instance;
        pass_list.num_commands = pass_cmds;
        layout->passes.push_back(std::move(pass_list));
    }
    current_layout = std::move(layout);
}

void BatchBuilder::StampLayoutSnapshot(uint8_t slot)
{
    slot_layouts[slot] = current_layout;
}

void BatchBuilder::BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm,
    BufferManager* bm, TextureManager* tm) {
    // Батчи ПЕРСИСТЕНТНЫ: пересобираются только на создание compute-программ. Замка нет, потому
    // что все программы создаются на инициализации, до старта потоков; появится создание в
    // рантайме — список придётся отдавать версией через shared_ptr, как BatchLayout.
    if (!sm || !sm->IsDirtyComputeBatches()) return;

    for (auto& rp : pass_manager->GetOrderedComputePasses()) {
        rp->shader_batches.clear();
    }
    for (auto& rp : pass_manager->GetOrderedComputePrepasses()) {
        rp->shader_batches.clear();
    }

    for (auto& slot : sm->GetComputeShaderPrograms()) {
        ComputeShaderProgram* sp = slot.program.get();
        if (!sp) continue;
        auto pipe = pm->GetComputePipeline(sp);
        if (!pipe) continue;

        ComputePassStep* cmp = pass_manager->GetComputePassStep(sp->compute_pass_name);
        // Пассы и препассы делят пространство имён (см. PassManager::CreateComputePass), поэтому
        // перебор «сначала пасс, потом препасс» однозначен.
        if (!cmp) cmp = pass_manager->GetComputePrepassStep(sp->compute_pass_name);
        if (!cmp) continue;

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
        auto resolve_atlases = [&](const std::vector<AtlasName>& names, const char* kind) {
            std::vector<TextureAtlas*> out;
            out.reserve(names.size());
            for (const AtlasName& n : names) {
                TextureAtlas* a = tm ? tm->GetTextureAtlas(n) : nullptr;
                if (!a) { SDL_Log("BuildComputeBatches '%s': %s atlas '%s' not found - binding slots will shift", slot.name.c_str(), kind, n.c_str()); continue; }
                out.push_back(a);
            }
            return out;
        };

        ComputeShaderBatchData new_batch{};
        new_batch.pipeline = pipe;
        new_batch.rw_storage_buffers = resolve_buffers(sp->rw_storage_buffer_names, "rw");
        new_batch.ro_storage_buffers = resolve_buffers(sp->ro_storage_buffer_names, "ro");

        new_batch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            TextureAtlas* a = tm ? tm->GetTextureAtlas(d.texture_atlas) : nullptr;
            if (!a) { SDL_Log("BuildComputeBatches '%s': rw atlas '%s' not found - binding slots will shift", slot.name.c_str(), d.texture_atlas.c_str()); continue; }
            new_batch.rw_storage_textures.push_back({ a, d.mip_level, d.layer });
        }
        new_batch.ro_storage_textures = resolve_atlases(sp->ro_storage_texture_names, "ro");
        new_batch.texture_binding     = resolve_atlases(sp->texture_sampler_names, "sampler");
        new_batch.push_instructions = sm->CollectComputePushInstructions(slot.name);
        new_batch.dispatch_func = sm->GetDispatchInstruction(slot.name);

        ComputeShaderData* csd = sm->GetComputeShader(sp->cs_name);
        if (!csd)
            SDL_Log("BuildComputeBatches '%s': compute shader '%s' not found in registry - dispatch falls back to 1x1x1",
                slot.name.c_str(), sp->cs_name.c_str());
        new_batch.threadcount_x = csd ? csd->threadcount_x : 1u;
        new_batch.threadcount_y = csd ? csd->threadcount_y : 1u;
        new_batch.threadcount_z = csd ? csd->threadcount_z : 1u;

        cmp->shader_batches.push_back(std::move(new_batch));
    }
    sm->SetDirtyComputeBatches(false);
}
