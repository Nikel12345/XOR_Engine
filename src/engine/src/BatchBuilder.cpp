#include "PCH.h"
#include "BatchBuilder.h"
#include "RenderCommandData.h"
#include "RenderSnapshot.h"
#include "ObjectManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "BufferManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "PositionStructure.h"
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

void BatchBuilder::SetDummyTexture(const std::string& name, TextureManager* tm)
{
    dummy_texture_name = name;
    // Сбор usage-флагов: dummy подставляется в слот материала (BuildBatches) → биндится сэмплером.
    // Своего материала у него нет, поэтому SAMPLER его атласу собирается тут, а не в MaterialManager.
    if (!tm) return;
    const auto& handles = tm->GetTextureHandles();
    auto it = handles.find(name);
    if (it == handles.end() || !it->second) return;
    TextureAtlas* atlas = it->second->atlas;
    if (!atlas) return;

    // Та же диагностика, что в MaterialManager::CollectSamplerUsage: опоздавшая декларация —
    // атлас уже СОЗДАН без SAMPLER, бинд упадёт абортом без имени ресурса (проверка ДО доливки).
    if (atlas->texture_binding.texture && !(atlas->tci.usage & SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "USAGE VIOLATION: dummy texture '%s' lives in atlas '%s', whose GPU texture was ALREADY "
            "CREATED without SDL_GPU_TEXTUREUSAGE_SAMPLER. It IS bound as a fragment sampler "
            "(fallback for missing material slots) - the bind will abort. "
            "Declare SAMPLER at atlas creation.",
            name.c_str(), atlas->debug_name.c_str());
    }

    atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;   // декларация: dummy биндится сэмплером
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

void BatchBuilder::AddEntityToBatches(Entity entity, PipeManager* pm, PassManager* pass_manager, TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm,
    const MaterialComponent& material_component, const ModelComponent& model_component) {

    // Модель и материалы у энтити — ССЫЛКИ ПО ИМЕНИ (см. ModelComponent/MaterialComponent);
    // резолвим здесь, на сборке, ровно как имена текстур/sp внутри материала ниже. Ищем прямо
    // в словаре, а НЕ через ModelManager::operator[] / MaterialManager::GetMaterial: те логируют
    // промах, а здесь вызов на КАЖДУЮ сущность — одно битое имя в сцене на 1М объектов дало бы
    // миллион строк лога. О пропуске сообщают гарды ниже (по одной строке на сущность).
    ModelData* model = nullptr;
    if (mdm && !model_component.name.empty()) {
        const auto& models = mdm->GetModels();
        auto it = models.find(model_component.name);
        if (it != models.end()) model = it->second.get();
    }

    // Защита от неразрешённых ссылок: пустое/неизвестное имя (ассет удалён, переименован или
    // ещё не создан) даёт nullptr. Без гарда разыменование model->submeshes падает на сборке.
    if (!model) {
        SDL_Log("BatchBuilder: entity %u has null model - skipped (unresolved asset '%s'?)",
            entity, model_component.name.c_str());
        return;
    }

    for (SubMeshData& submesh : model->submeshes)
    {
        if (material_component.names.size() != model->submeshes.size()) {
            SDL_Log("BulidBatches:: Submash and material sizes mismatch");
        }
        // Границы + промах имени материала (та же природа, что у модели выше).
        if (submesh.material_index >= material_component.names.size()) {
            SDL_Log("BatchBuilder: entity %u material_index out of range - skipped", entity);
            continue;
        }
        const std::string& material_name = material_component.names[submesh.material_index];
        Material* material = nullptr;
        if (mtm && !material_name.empty()) {
            const auto& materials = mtm->GetMaterials();
            auto mit = materials.find(material_name);
            if (mit != materials.end()) material = mit->second.get();
        }
        if (!material) {
            SDL_Log("BatchBuilder: entity %u has null material - skipped (unresolved asset '%s'?)",
                entity, material_name.c_str());
            continue;
        }

        for (const ShaderName& sp_name : material->shader_programs)
        {
            // name-based ссылка: имя sp → указатель на сборке батча. Промах (sp удалена) → fallback-sp
            // (аналог textureless), а если и его нет — пропуск.
            ShaderProgram* sp = sm ? sm->GetShaderProgram(sp_name) : nullptr;
            if (!sp) {
                // sp удалена → fallback ПО ИМЕНИ (резолвим как обычную sp; удалён и он → пустой рендер, без краша).
                SDL_Log("BatchBuilder::Material references deleted shader program '%s' - using fallback", sp_name.c_str());
                sp = (sm && !fallback_shader_name.empty()) ? sm->GetShaderProgram(fallback_shader_name) : nullptr;
                if (!sp) continue;
            }
            RenderPassStep* rp = pass_manager->GetRenderPassStep(sp->render_pass_name);
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
                // Вершинные СТРИМЫ пула — из объявления вершинника (vs.vertex_buffer_names,
                // порядок = слоты пайплайна). Резолв здесь же; пустой список = vs не найден или
                // стрим-имя протухло → бинд-шаг пропустит draw (сдвиг слота = UB).
                // Индексный буфер — принадлежность ПУЛА: у одного пула он один, и vs запомнил его
                // на создании. Читаем поле, а не ищем пул: реестр пулов живёт в ModelManager,
                // которого у сборки батча нет и быть не должно.
                if (VertexShaderData* vsd = sm->GetVertexShader(sp->vs_name)) {
                    new_batch.vertexBuffers = resolve_buffers(vsd->vertex_buffer_names);
                    if (vsd->index_buffer)
                        new_batch.indexBuffer = bm->GetBufferData(vsd->index_buffer);
                }
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
                    SDL_Log("BuildBatches:: dummy texture missing too - sp draw skipped for this material");
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
                    SDL_Log("BuildBatches:: dummy texture missing too - sp draw skipped for this material");
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
                new_model.pib_sub_buffer.reserve(16);
                model_map[model_key] = std::move(new_model);
            }

            ModelBatchData& model_batch = model_map[model_key];

            uint32_t slot_index = safe_u32(model_batch.pib_sub_buffer.size());
            model_batch.instanceCount++;
            model_batch.pib_sub_buffer.push_back(entity);
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
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, SceneData* scene)
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
        BuildRenderBatches(pm, pass_manager, om, tm, sm, bm, mdm, mtm, scene);
        FinalizeOffsets(pass_manager, bm);
        // Полная пересборка переклеила ВСЮ раскладку (indirect_command_index, firstInstance).
        // Бампим эпоху — слоты, залитые под старой раскладкой, рендер больше не покажет.
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
    TextureManager* tm, ShaderManager* sm, BufferManager* bm,
    ModelManager* mdm, MaterialManager* mtm, SceneData* scene)
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
        entities_to_update.clear();
    }

    // Отбор по маркеру DrawComponent — Positions НЕ требуется (тот же критерий, что в
    // ApplyIncremental). Transformless-дровабл (напр. скайбокс: его VS строит позицию из
    // камеры) батчится как все, но строки трансформа не имеет — StorePIB пишет ему -1,
    // каллинг скаттерит такую запись безусловно. Геометрия/материал не часть сигнатуры —
    // тянем через Has/GetComponent.
    om->ForEach<DrawComponent>(
        scene,
        [&](Entity entity, const DrawComponent& draw)
    {
        if (!draw.visible) return;  // скрытые (выключенные в UI debug-рамки) в дерево не идут
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
    // Atomically take + clear the queues, then work on the local copies outside
    // the lock so we never hold delta_mutex while mutating the batch tree.
    std::vector<Entity> creates, deletes, updates;
    {
        std::lock_guard<std::mutex> lock(delta_mutex);
        creates.swap(entities_to_create);
        deletes.swap(entities_to_delete);
        updates.swap(entities_to_update);
    }
    if (creates.empty() && deletes.empty() && updates.empty()) return false;

    // Add-сторона общая для create и update: отбор рисуемого — тот же, что в BuildRenderBatches
    // (есть модель и материал; есть DrawComponent и он visible — флаг источник истины).
    auto add_if_drawable = [&](Entity entity) {
        if (!om->Has<ModelComponent>(scene, entity) || !om->Has<MaterialComponent>(scene, entity))
            return;
        if (!om->Has<DrawComponent>(scene, entity) || !om->GetComponent<DrawComponent>(scene, entity).visible)
            return;
        const MaterialComponent& material_component = om->GetComponent<MaterialComponent>(scene, entity);
        const ModelComponent& model_component = om->GetComponent<ModelComponent>(scene, entity);
        AddEntityToBatches(entity, pm, pass_manager, tm, sm, bm, mdm, mtm, material_component, model_component);
    };

    // Перевесить — ПЕРВЫМИ и в обход обоих гардов create-стороны: энтити жива, просто её место
    // в дереве изменилось. Снять со старых слотов и добавить по текущим компонентам. Порядок
    // важен: после этого она уже в дереве, поэтому парный QueueCreate (если он был) погасится
    // гардом идемпотентности, а парный QueueDelete отработает ниже и уберёт её целиком.
    for (Entity entity : updates) {
        RemoveEntityFromBatches(entity);
        add_if_drawable(entity);
    }

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
    uint32_t command_index = 0;

    // Одним обходом: проставляем офсеты в ДЕРЕВЕ (его читают sim-модули — indirect/PIB
    // при заливке буферов) и строим НОВУЮ версию СЛЕПКА раскладки (RenderSnap::BatchLayout) —
    // значения-двойник дерева для рендера. Слоты получают её в StampLayoutSnapshot: рендер
    // слота k рисует ровно по раскладке, под которую залит его indirect_buffer[k].
    auto layout = std::make_shared<RenderSnap::BatchLayout>();
    layout->passes.reserve(pass_manager->GetOrderedRenderPasses().size());
    // Индирект-буфер раскладки — свойство всей раскладки (сквозная нумерация команд), резолв
    // здесь: цикл отрисовки не лазит в реестр по имени на каждый texture batch.
    layout->indirectBuffer = bm->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER);

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses())
    {
        RenderSnap::PassDrawList pass_list;
        pass_list.first_instance = offset;
        pass_list.shaders.reserve(rp->shader_batches.size());

        // Глобальные сэмплеры прохода (тень/env): резолвим СТАБИЛЬНЫЕ атласы в актуальные
        // SDL-биндинги ЗДЕСЬ, значениями в слепок — как это делает compute на диспатче. В цикле
        // отрисовки резолвить нечего: по шейдер-батчам прохода значение постоянно. Атлас без
        // GPU-текстуры пропускаем (иначе забиндили бы null и сдвинули слоты батчевых сэмплеров).
        pass_list.global_texture_bindings.reserve(rp->global_texture_bindings.size());
        for (TextureAtlas* atlas : rp->global_texture_bindings) {
            if (!atlas || !atlas->texture_binding.texture) {
                SDL_Log("BatchBuilder: pass '%s' - global sampler atlas is null/has no GPU texture, skipped.", rp->debug_name.c_str());
                continue;
            }
            pass_list.global_texture_bindings.push_back(atlas->texture_binding);
        }

        for (auto& [shader_key, shader_batch] : rp->shader_batches)
        {
            RenderSnap::ShaderGroup sg;
            sg.pipeline = shader_batch.pipeline;
            sg.push_func = shader_batch.push_func;
            sg.vertexBuffers = shader_batch.vertexBuffers;
            sg.indexBuffer = shader_batch.indexBuffer;
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

void BatchBuilder::BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm,
    BufferManager* bm, TextureManager* tm) {
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

    for (auto& slot : sm->GetComputeShaderPrograms()) {
        ComputeShaderProgram* sp = slot.program.get();
        if (!sp) continue;
        SDL_GPUComputePipeline* pipe = pm->GetComputePipeline(sp);
        if (!pipe) continue;

        // Пассы и препассы делят пространство имён (см. PassManager::CreateComputePass),
        // поэтому «сначала пасс, иначе препасс» однозначно.
        ComputePassStep* cmp = pass_manager->GetComputePassStep(sp->compute_pass_name);
        if (!cmp) cmp = pass_manager->GetComputePrepassStep(sp->compute_pass_name);
        if (!cmp) continue;

        // Резолв имён в указатели — ЗДЕСЬ (csp хранит только имена, чтобы сериализоваться).
        // Промах = пропуск ресурса, а не пропуск программы: слоты бинда съедут, поэтому громко логируем.
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

        // Атласы СТАБИЛЬНЫ (резолв SDL_GPUTexture* — позже, в ComputePassStandardBody): ресайз
        // пересоздаёт текстуру внутри того же TextureAtlas, указатель на обёртку переживает его.
        new_batch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            TextureAtlas* a = tm ? tm->GetTextureAtlas(d.texture_atlas) : nullptr;
            if (!a) { SDL_Log("BuildComputeBatches '%s': rw atlas '%s' not found - binding slots will shift", slot.name.c_str(), d.texture_atlas.c_str()); continue; }
            new_batch.rw_storage_textures.push_back({ a, d.mip_level, d.layer });
        }
        new_batch.ro_storage_textures = resolve_atlases(sp->ro_storage_texture_names, "ro");
        new_batch.texture_binding     = resolve_atlases(sp->texture_sampler_names, "sampler");   // даёт texture+sampler
        new_batch.push_func = sp->push_func;
        new_batch.dispatch_func = sp->dispatch_func;

        ComputeShaderData* csd = sm->GetComputeShader(sp->cs_name);   // cs по имени из реестра
        if (!csd)
            SDL_Log("BuildComputeBatches '%s': compute shader '%s' not found in registry - dispatch falls back to 1x1x1",
                slot.name.c_str(), sp->cs_name.c_str());
        new_batch.threadcount_x = csd ? csd->threadcount_x : 1u;
        new_batch.threadcount_y = csd ? csd->threadcount_y : 1u;
        new_batch.threadcount_z = csd ? csd->threadcount_z : 1u;

        cmp->shader_batches.push_back(std::move(new_batch));
    }
    sm->SetDirtyComputeBatches(false);
    // Дерево пересобрано — старых указателей пайплайнов в нём больше нет. Бамп армирует
    // отложенное удаление compute-пайплайнов (TrashPipelines дренирует in-flight и освобождает).
    compute_rebuild_epoch.fetch_add(1, std::memory_order_release);
}
