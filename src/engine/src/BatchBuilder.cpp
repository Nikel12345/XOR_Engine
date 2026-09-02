#include "PCH.h"
#include "BatchBuilder.h"
#include "BaseComponents.h"
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

// Финальное перемешивание (murmur3 fmix64) — общий хвост всех ключей ниже.
static inline uint64_t MixKey(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

// Ключ ПАМЯТКИ предпрохода: идентичность пары (материал, sp) плюс блоб ЭТОЙ sp. Блоб в ключе,
// потому что две ячейки одного материала могут отрезолвиться в ОДНУ sp (обе упали на fallback),
// а данные у них разные — памятка обязана их различать, иначе вторая получила бы пуш первой.
// Это НЕ ключ батча: узлы дерева ключуются ресурсами (HashMatSpResources), а не адресами.
MatSpKey HashMatSpMemo(const Material* mat, const ShaderProgram* sp,
                       const std::vector<uint8_t>* params) {
    MatSpKey key = reinterpret_cast<MatSpKey>(mat);
    key = MixKey(key ^ reinterpret_cast<MatSpKey>(sp));
    return MixKey(key ^ reinterpret_cast<MatSpKey>(params));
}

// Ключ texture-батча = РЕСУРСЫ, которые потребляет ИМЕННО ЭТА sp, и ничего сверх того:
//   • ВСЕ текстуры её required_slots — не только дефолт слота, но и его варианты (хэндлы:
//     тот же атлас, но другой UVL — уже другой узел);
//   • адресация этой таблицы (слова slot_layout: base/cell/count);
//   • адрес блоба params ЭТОЙ sp (идентичность, не содержимое).
// Материал целиком в ключ не входит: sp, которая от материала не берёт ничего (ShadowCaster,
// Wireframe), не различает материалы вовсе — все они дают ей один узел, один draw и одну
// команду индиректа. Раньше ключом был адрес материала, и теневой проход дробился по нему же.
//
// ПРАВИЛО: всё, что уходит в ПУШ, обязано входить сюда. Ключ не содержит идентичности материала,
// поэтому два разных материала схлопываются в один узел — и если их таблицы или адресация
// различаются, второй получит пуш первого (нормалка сэмплится как альбедо, без строки в логе).
// Отсюда и варианты, и слова адресации в ключе.
//
// Хэндлы приходят СПИСКОМ, а не резолвятся заново по именам: ключ обязан совпадать с таблицей
// поблочно, а правила подстановки (промах варианта — выкинуть, промах дефолта — dummy) живут в
// сборке таблицы. Повторить их здесь = завести вторую копию правила, которая разойдётся.
// Порядок блоков в списке = порядок блоков в uvl.
//
// Почему params по АДРЕСУ, а не по содержимому: правка байт в инспекторе (ползунок цвета) не
// должна менять ключ, иначе дерево батчей пересобиралось бы покадрово. Адрес блоба стабилен —
// он в куче и переживает реаллокацию ячеек (см. SpBinding::params).
// Пустой блоб = «параметров нет» → в ключ не вносится (ClearMaterialParams гасит байты,
// не освобождая память).
MatSpKey HashMatSpResources(const ShaderProgram* sp, const std::vector<uint8_t>* params,
                            const uint32_t* slot_words,
                            const std::vector<const TextureHandle*>& block_handles) {
    if (!sp) {
        SDL_Log("HashMatSpResources: shader program is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    MatSpKey key = 0;
    const size_t slot_count = std::min<size_t>(sp->required_slots.size(), MAX_SLOTS);
    for (size_t s = 0; s < slot_count; ++s) {
        // Роль и её адресация — в ключ ВСЕГДА, в т.ч. когда текстуры нет: «нет текстуры» —
        // такое же состояние узла, как конкретный хэндл (батч соберёт сюда dummy).
        key += static_cast<MatSpKey>(sp->required_slots[s]) + 0x9e3779b97f4a7c15ull;
        key ^= static_cast<MatSpKey>(slot_words[s]);
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

// Вторая половина ключа texture-батча — единственное, что зависит от СУЩНОСТИ: каким по счёту
// материалом этот батч идёт у неё. Номер уходит в пуш (смещение секции состояний считается как
// material_index * MAX_VARIATIVE_SLOTS), значит по правилу выше обязан быть в ключе.
// Дробление ограничено максимальным числом сабмешей у модели, а не числом материалов в сцене.
TextureBatchKey HashTextureBatchKey(MatSpKey res_key, uint32_t material_index) {
    return MixKey(res_key ^ (static_cast<TextureBatchKey>(material_index) + 0x9e3779b97f4a7c15ull));
}

// Ключ atlas-батча = АТЛАСЫ слотов, то есть ровно то, что биндится сэмплерами. Атласы ВСЕХ
// блоков, а не только дефолтов: инвариант «все варианты слота в одном атласе» — варнинг, а не
// отказ (см. EngineContext::CreateMaterial), и при его нарушении вариант ≥1 иначе молча
// сэмплился бы из чужого атласа. От material_index не зависит вовсе.
AtlasBatchKey HashAtlasBatchKey(const ShaderProgram* sp,
                                const std::vector<const TextureHandle*>& block_handles) {
    if (!sp) {
        SDL_Log("HashAtlasBatchKey: shader program is nullptr!");
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

void BatchBuilder::BuildMaterialLayouts(TextureManager* tm, ShaderManager* sm, MaterialManager* mtm)
{
    mat_sp_layouts.clear();
    if (!mtm || !sm) return;

    // Dummy и fallback-sp — ПО ИМЕНИ, через карту (Get* логировал бы промах). Резолвим один раз
    // на весь предпроход: это те же подстановки, что раньше делались на каждой сущности.
    TextureHandle* dummy = nullptr;
    if (tm && !dummy_texture_name.empty()) {
        auto dit = tm->GetTextureHandles().find(dummy_texture_name);
        if (dit != tm->GetTextureHandles().end()) dummy = dit->second.get();
    }
    if (dummy && !dummy->atlas) dummy = nullptr;   // без атласа он ничего не заменяет
    ShaderProgram* fallback = fallback_shader_name.empty() ? nullptr : sm->GetShaderProgram(fallback_shader_name);

    // Хэндлы блоков текущей таблицы — переиспользуемый буфер, чтобы предпроход не аллоцировал
    // на каждую пару.
    std::vector<const TextureHandle*> block_handles;

    for (const auto& [mat_name, mat_owner] : mtm->GetMaterials()) {
        Material* material = mat_owner.get();
        if (!material) continue;

        // Ячейки секции состояний: порядок — ОДНО определение на весь движок
        // (CollectVariativeRoles в MaterialData.h), его же читает TextureStateDataModule при
        // заливке. Расходиться им нельзя — объекты молча покажут чужие варианты, поэтому цикл
        // здесь не переписывается, а вызывается. Переполнение функция не логирует (у неё нет
        // имени материала) — сообщение здесь, одна строка на материал.
        const VariativeRoles cells = CollectVariativeRoles(*material);
        if (cells.count >= MAX_VARIATIVE_SLOTS)
            SDL_Log("BuildMaterialLayouts: material '%s' may have more variative slots than "
                    "MAX_VARIATIVE_SLOTS (%u) - the rest show their default only (raise the constant)",
                mat_name.c_str(), MAX_VARIATIVE_SLOTS);

        for (const SpBinding& binding : material->shader_programs) {
            const std::vector<uint8_t>* sp_params =
                (binding.params && !binding.params->empty()) ? binding.params.get() : nullptr;
            // Промах имени sp → fallback, как в AddEntityToBatches (там же и лог о промахе).
            ShaderProgram* sp = sm->GetShaderProgram(binding.sp);
            if (!sp) sp = fallback;
            if (!sp) continue;

            const MatSpKey memo = HashMatSpMemo(material, sp, sp_params);
            if (mat_sp_layouts.count(memo)) continue;   // две ячейки упали на одну sp с одним блобом

            if (sp->required_slots.size() > MAX_SLOTS)
                SDL_Log("BuildMaterialLayouts: sp '%s' declares %zu texture slots, MAX_SLOTS is %u - "
                        "slots beyond it get no layout word and cannot be addressed (raise the constant)",
                    sp->debug_name.c_str(), sp->required_slots.size(), MAX_SLOTS);

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

                const uint32_t base = safe_u32(lay.uvl.size());   // таблица сгруппирована по слотам

                // ── Дефолт слота (вариант 0) ── Место в таблице сохраняет ВСЕГДА: на нём стоит
                // base следующих слотов. Промах имени подменяется dummy; нет и его — sp у этого
                // материала не рисуется вовсе (пустой рендер вместо мёртвого хэндла).
                TextureHandle* def = (names && !names->empty() && tm)
                    ? tm->GetTextureHandle((*names)[0]) : nullptr;
                if (!def || !def->atlas) {
                    SDL_Log("BuildMaterialLayouts: material '%s' has no resolvable texture for slot %d "
                            "of sp '%s' - dummy is used", mat_name.c_str(), static_cast<int>(role),
                        sp->debug_name.c_str());
                    def = dummy;
                }
                if (!def) { lay.bindable = false; break; }

                lay.uvl.push_back(MakeUVL(def->texture_data));   // КОПИЯ значения (см. инвариант в TextureBatchData)
                block_handles.push_back(def);
                // Бинд слота — атлас его ДЕФОЛТА: варианты обязаны лежать там же (варнинг на
                // создании материала), поэтому один Texture2DArray на слот покрывает их все.
                lay.texture_binding.push_back(def->atlas->texture_binding);

                // ── Варианты (1..N) ── Неразрешимое имя в таблицу НЕ попадает и count не растёт:
                // иначе base всех последующих слотов разъехался бы с реальной таблицей.
                uint32_t count = 1;
                uint32_t cell = 0;
                // Ячейка есть только у роли, попавшей в нумерацию выше; гард MAX_VARIATIVE_SLOTS
                // мог её срезать — тогда слот остаётся невариативным и показывает дефолт.
                // has_cell ⇒ names непуст и в нём больше одного имени: cells строились из него же.
                bool has_cell = false;
                for (uint32_t c = 0; c < cells.count; ++c)
                    if (cells.role[c] == role) { cell = c; has_cell = true; break; }

                if (has_cell) {
                    if (lay.uvl.size() + names->size() - 1 > MAX_UVL_BLOCKS) {
                        SDL_Log("BuildMaterialLayouts: material '%s' + sp '%s': UVL table would exceed "
                                "MAX_UVL_BLOCKS (%u) - slot %d shows its default only (raise the constant)",
                            mat_name.c_str(), sp->debug_name.c_str(), MAX_UVL_BLOCKS, static_cast<int>(role));
                    }
                    else for (size_t v = 1; v < names->size(); ++v) {
                        TextureHandle* h = tm ? tm->GetTextureHandle((*names)[v]) : nullptr;
                        if (!h || !h->atlas) continue;   // GetTextureHandle уже назвал промах в логе
                        lay.uvl.push_back(MakeUVL(h->texture_data));
                        block_handles.push_back(h);
                        ++count;
                    }
                }
                if (count == 1) cell = 0;   // невариативный слот ячейку не занимает
                else            lay.variative = true;   // узел реально читает состояние (см. MatSpLayout::variative)

                // Материал БЕЗ вариантов обязан давать сегодняшнюю таблицу байт-в-байт: один
                // блок на слот, base[s] == s. Вырождение в прежнее поведение — главное свойство
                // раскладки, и ловится оно тут одной строкой (в Release её нет).
                assert((cells.count != 0 || (count == 1 && base == safe_u32(s)))
                    && "BuildMaterialLayouts: material without variants must yield the legacy UVL table");

                if (s < MAX_SLOTS)
                    lay.slot[s] = (base << 16) | (cell << 8) | count;
            }

            if (!lay.bindable) {
                SDL_Log("BuildMaterialLayouts: material '%s' + sp '%s': a required slot has neither a "
                        "texture nor a dummy - this sp draw is skipped for the material",
                    mat_name.c_str(), sp->debug_name.c_str());
                lay.uvl.clear();
                lay.texture_binding.clear();
            }
            else {
                lay.res_key   = HashMatSpResources(sp, sp_params, lay.slot, block_handles);
                lay.atlas_key = sp->required_slots.empty() ? 0 : HashAtlasBatchKey(sp, block_handles);
            }
            mat_sp_layouts.emplace(memo, std::move(lay));
        }
    }
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

    // Сверка длин — ОДИН раз на сущность: внутри цикла она печатала строку на каждый сабмеш,
    // и город на 3000 домов давал десятки тысяч строк за сборку.
    if (material_component.materials.size() != model->submeshes.size()) {
        SDL_Log("BulidBatches:: Submash and material sizes mismatch (entity %u, model '%s': %u materials, %u submeshes)",
            entity, model_component.name.c_str(),
            safe_u32(material_component.materials.size()), safe_u32(model->submeshes.size()));
    }

    for (SubMeshData& submesh : model->submeshes)
    {
        // Сабмеш без индексов рисовать нечем, но узел батча он заводил полноценный: свою
        // индирект-команду с num_indices == 0 И СВОИ ИНСТАНСЫ в out_pib, которые кулинг честно
        // обрабатывает. Пустые слоты — норма (модель обязана нести все номера сабмешей, иначе
        // сдвинется адресация материалов), поэтому отсекаем их здесь, до узла.
        if (submesh.indexCount == 0) continue;

        // Границы + промах имени материала (та же природа, что у модели выше).
        if (submesh.material_index >= material_component.materials.size()) {
            SDL_Log("BatchBuilder: entity %u material_index out of range - skipped", entity);
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
            SDL_Log("BatchBuilder: entity %u has null material - skipped (unresolved asset '%s'?)",
                entity, material_name.c_str());
            continue;
        }

        for (const SpBinding& binding : material->shader_programs)
        {
            const ShaderName& sp_name = binding.sp;
            // Блоб этой sp: пустой (или отсутствующий) = она params не читает — ни пуша, ни ключа.
            const std::vector<uint8_t>* sp_params =
                (binding.params && !binding.params->empty()) ? binding.params.get() : nullptr;
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

            // Пер-материальная половина батча — из памятки предпрохода (BuildMaterialLayouts):
            // таблица UVL, её адресация, бинды атласов и обе половины ключей уже посчитаны, и
            // резолвить имена текстур на каждую сущность больше не нужно. Промах = материал/sp
            // появились после предпрохода (тот идёт в начале того же UpdateRenderBatches) —
            // такого быть не должно, но узел без раскладки собирать нечем.
            auto lay_it = mat_sp_layouts.find(HashMatSpMemo(material, sp, sp_params));
            if (lay_it == mat_sp_layouts.end()) continue;
            const MatSpLayout& lay = lay_it->second;
            if (!lay.bindable) continue;   // причину назвал предпроход, по строке на материал

            auto& atlas_map = sb.atlases_batches;
            auto atlas_it = atlas_map.find(lay.atlas_key);
            if (atlas_it == atlas_map.end())
            {
                AtlasBatchData new_tex{};
                new_tex.texture_binding = lay.texture_binding;
                atlas_map[lay.atlas_key] = std::move(new_tex);
            }

            AtlasBatchData& atlas_batch = atlas_map[lay.atlas_key];

            // Единственное, что зависит от сущности: каким по счёту материалом идёт этот батч.
            // Невариативному узлу он не нужен — гард count > 1 не пустит чтение состояния, —
            // поэтому там он равен нулю и в ключ ничего не вносит: узел не дробится по сабмешам.
            const uint32_t material_index = lay.variative ? submesh.material_index : 0u;
            TextureBatchKey tex_key = HashTextureBatchKey(lay.res_key, material_index);

            auto& tex_map = atlas_batch.texture_batches;
            auto texb_it = tex_map.find(tex_key);
            if (texb_it == tex_map.end()) {
                TextureBatchData new_texb{};
                // Данные ЭТОЙ sp — и только они: узел, собранный для sp без params, никаких
                // чужих байт не носит. Само РЕШЕНИЕ пушить остаётся за RenderManager (по числу
                // fragment-uniform'ов шейдера), здесь — адресат.
                new_texb.params = sp_params;
                new_texb.texture_uvl = lay.uvl;
                std::copy(std::begin(lay.slot), std::end(lay.slot), std::begin(new_texb.variant_layout.slot));
                new_texb.variant_layout.material_index = material_index;

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
    // Нет активной сцены — собирать нечего, дерево батчей остаётся пустым (кадр рисуется чёрным).
    // МОЛЧА: зовётся каждый кадр, и лог тут давал сотни строк в секунду об одном и том же, забивая
    // и настоящую диагностику, и консоль. О самом состоянии один раз сообщает ObjectManager::
    // GetActiveScene — там же, где оно возникает.
    if (!scene) return;

    // Either a full rebuild (scene activation) or an incremental delta — never
    // both. exchange(false) consumes the rebuild request atomically.
    // Замок дерева больше не нужен: рендер живое дерево не читает (рисует по слепку
    // раскладки слота — см. FinalizeOffsets/AskLayout), дерево приватно для sim.
    // Предпроход — ДО развилки: AddEntityToBatches общая для полной пересборки и инкремента,
    // и обе стороны читают памятку. Материалов десятки, поэтому полный обход дешевле ветки
    // «есть ли уже в памятке» в цикле на миллион сущностей; на инкрементальном пути таблица
    // соберётся и умрёт вместе с вызовом — бесполезно, но и бесплатно.
    BuildMaterialLayouts(tm, sm, mtm);

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

        // Индекс команды в слепке — ЛОКАЛЬНЫЙ для прохода: регион прохода в индиректе содержит
        // только его команды, и дроу адресует их от базы своего региона (см. culling_fix.md).
        // Никакого внешнего знания для нумерации не нужно — счётчик просто свой на проход.
        uint32_t pass_cmd_index = 0;

        // Счётчик формы дерева: draw = texture-батч (одна SDL_DrawGPUIndexedPrimitivesIndirect),
        // cmd = model-батч (одна команда мультидроу внутри неё). Печатается ТОЛЬКО отсюда,
        // то есть на изменение дерева, а не покадрово. Нужен, чтобы правка ключей была видна
        // числом: лишний хэш в ключе дробит узлы, и это единственный симптом — картинка та же.
        uint32_t pass_draws = 0, pass_cmds = 0;

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
                    texture_batch.indirect_command_index = pass_cmd_index;

                    RenderSnap::TextureDraw td;
                    td.texture_uvl = texture_batch.texture_uvl;
                    td.variant_layout = texture_batch.variant_layout;
                    td.params = texture_batch.params;   // невладеющий, адрес стабилен (см. RenderSnapshot.h)
                    td.indirect_command_index = pass_cmd_index;
                    td.draw_count = safe_u32(texture_batch.model_batches.size());

                    for (auto& [model_key, model_batch] : texture_batch.model_batches)
                    {
                        model_batch.firstInstance = offset;
                        offset += model_batch.instanceCount;
                        pass_cmd_index++;
                    }
                    ++pass_draws;
                    pass_cmds += td.draw_count;
                    ag.draws.push_back(std::move(td));
                }
                sg.atlases.push_back(std::move(ag));
            }
            pass_list.shaders.push_back(std::move(sg));
        }
        SDL_Log("[batch] pass '%s': draws=%u cmds=%u shaders=%zu instances=%u",
            rp->debug_name.c_str(), pass_draws, pass_cmds,
            rp->shader_batches.size(), offset - pass_list.first_instance);

        // Суммы прохода: из них StampRegions складывает размеры его региона (записей и команд
        // на камеру) и границы его сегмента во входном PIB для диапазона каллинга.
        pass_list.num_instances = offset - pass_list.first_instance;
        pass_list.num_commands = pass_cmds;
        layout->passes.push_back(std::move(pass_list));
    }
    current_layout = std::move(layout);
}

// Слепок раскладки слоту — O(1). Зовётся в PrepareFunc СРАЗУ после UpdateRenderBatches,
// до заливки буферов слота: всё, что prepare зальёт (indirect/PIB/out_pib), соответствует
// именно этой версии раскладки.
void BatchBuilder::StampLayoutSnapshot(uint8_t slot)
{
    slot_layouts[slot] = current_layout;
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
