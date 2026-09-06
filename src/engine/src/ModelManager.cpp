#include "PCH.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include "ModelManager.h"
#include "BufferManager.h"

ModelManager::ModelManager() {};

// Ёмкости на заведение пула: обе растут сами (RESIZE_AND_COPY), это лишь стартовый размер.
static constexpr uint32_t BASE_VERTEX_CAPACITY = 186150;    // вершин на стрим
static constexpr uint32_t BASE_INDEX_CAPACITY = 2047501;    // индексов (uint32)

struct SubMeshFileEntry {
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t material_index;
};

// Позиция вершины из байтового стейджинга. memcpy, а не reinterpret_cast: раскладка приходит
// извне, и выравнивание её вершины/офсета позиции ничем не гарантировано.
static inline glm::vec3 ReadPos(const std::byte* base, uint32_t vsize, uint32_t pos_off, size_t i)
{
    glm::vec3 p;
    std::memcpy(&p, base + i * vsize + pos_off, sizeof(glm::vec3));
    return p;
}

static inline void WritePos(std::byte* base, uint32_t vsize, uint32_t pos_off, size_t i, const glm::vec3& p)
{
    std::memcpy(base + i * vsize + pos_off, &p, sizeof(glm::vec3));
}

// Строит сабмеши модели из записей entries: смещения ОТНОСИТЕЛЬНО стейджинга + bounding sphere по
// вершинам оттуда же. Общее для диска и генератора.
// Смещения именно относительные: абсолютная база — начало диапазона, который аллокатор выдаст
// пачке при заливке, а он неизвестен до неё. Переводит их UploadModelIndexBuffer (финализатор).
// Раскладка без читаемой позиции (нет POSITION либо он упакован) — законна: сабмеши тогда
// получают вырожденную сферу w=-1, и каллинг трактует такую модель как «видима всегда»
// (culling_pib.comp.hlsl: has_geom = sphere.w >= 0). Границы для неё обязаны прийти из данных.
static void BuildSubmeshes(const std::byte* staging, const GeometryPool* pool, ModelData* model,
    const std::vector<SubMeshFileEntry>& entries, uint32_t vbase, uint32_t ibase)
{
    const bool     has_pos = pool->HasFloat3Position();
    const uint32_t vsize = pool->VertexSize();
    const uint32_t pos_off = has_pos ? pool->PositionOffset() : 0;

    model->submeshes.clear();
    model->submeshes.reserve(entries.size());
    for (const SubMeshFileEntry& e : entries) {
        SubMeshData sub{};
        sub.vertexOffset = vbase + e.vertexOffset;
        sub.indexOffset = ibase + e.indexOffset;
        sub.vertexCount = e.vertexCount;
        sub.indexCount = e.indexCount;
        sub.material_index = e.material_index;
        sub.sphere = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);

        if (e.vertexCount > 0 && has_pos) {
            const size_t first = size_t(vbase) + e.vertexOffset;
            const glm::vec3 p0 = ReadPos(staging, vsize, pos_off, first);
            glm::vec3 center(0.0f);
            glm::vec3 mn = p0, mx = p0;
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                const glm::vec3 p = ReadPos(staging, vsize, pos_off, first + k);
                center += p;
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            center /= static_cast<float>(e.vertexCount);

            float radius = 0.0f;
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                float d = glm::distance(center, ReadPos(staging, vsize, pos_off, first + k));
                if (d > radius) radius = d;
            }
            sub.sphere = glm::vec4(center, radius);
            // Локальный AABB сабмеша (для авто-боксов коллайдера в ColliderQuery).
            sub.aabb_center = (mn + mx) * 0.5f;
            sub.aabb_half = (mx - mn) * 0.5f;
        }
        model->submeshes.push_back(sub);
    }
}

// Запекает пивот в вершины: один проход min/max по диапазону [vbase, vbase+vcount),
// switch выбирает точку q из локального AABB, и геометрия сдвигается на -q. Вызывается
// ДО BuildSubmeshes — тогда sphere/AABB сабмешей считаются уже от нового origin.
// Раскладке без записываемой FLOAT3-позиции пивот недоступен в принципе — вызывающий обязан
// отсеять такой случай раньше (см. _LoadModelFile).
static void ApplyAnchorShift(std::byte* staging, const GeometryPool* pool, size_t vbase, uint32_t vcount, AnchorShift anchor)
{
    if (anchor == AnchorShift::Keep || vcount == 0 || !pool->HasFloat3Position()) return;

    const uint32_t vsize = pool->VertexSize();
    const uint32_t pos_off = pool->PositionOffset();

    glm::vec3 mn = ReadPos(staging, vsize, pos_off, vbase), mx = mn;
    for (uint32_t k = 1; k < vcount; ++k) {
        const glm::vec3 p = ReadPos(staging, vsize, pos_off, vbase + k);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }

    glm::vec3 q;
    switch (anchor) {
    case AnchorShift::Center: q = (mn + mx) * 0.5f;  break;
    case AnchorShift::LBB: q = { mn.x, mn.y, mn.z };  break;
    case AnchorShift::RBB: q = { mx.x, mn.y, mn.z };  break;
    case AnchorShift::LTB: q = { mn.x, mx.y, mn.z };  break;
    case AnchorShift::RTB: q = { mx.x, mx.y, mn.z };  break;
    case AnchorShift::LBF: q = { mn.x, mn.y, mx.z };  break;
    case AnchorShift::RBF: q = { mx.x, mn.y, mx.z };  break;
    case AnchorShift::LTF: q = { mn.x, mx.y, mx.z };  break;
    case AnchorShift::RTF: q = { mx.x, mx.y, mx.z };  break;
    default: q = glm::vec3(0.0f);                     break;
    }

    for (uint32_t k = 0; k < vcount; ++k)
        WritePos(staging, vsize, pos_off, vbase + k,
                 ReadPos(staging, vsize, pos_off, vbase + k) - q);
}

// ── Пулы геометрии ─────────────────────────────────────────────────────────────────────────

GeometryPool* ModelManager::CreateGeometryPool(BufferManager* bm, const std::string& name, uint32_t vertex_size,
    const std::vector<GeometryPool::StreamDesc>& streams)
{
    if (auto it = pools.find(name); it != pools.end()) {
        SDL_Log("Geometry pool '%s' already exists, returning existing pool.", name.c_str());
        return it->second.get();
    }
    if (!bm || name.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CreateGeometryPool: null BufferManager or empty name.");
        return nullptr;
    }

    auto owned = std::make_unique<GeometryPool>(name, vertex_size, streams);
    GeometryPool* pool = owned.get();
    if (pool->Streams().empty()) {   // конструктор уже отругался — не заводим полупустой пул
        return nullptr;
    }
    pools[name] = std::move(owned);
    residency[pool];                 // пустая запись дозагрузки
    if (!default_pool) default_pool = pool;

    // Только РЕГИСТРАЦИЯ обёрток: SDL_GPUBuffer сделает BakePending, и только когда стримы назовёт
    // вершинник (usage). Пул, которым никто не пользуется, не занимает ни байта VRAM.
    for (const GeometryPool::Stream& s : pool->Streams())
        bm->CreateBufferData(s.buffer_name, BASE_VERTEX_CAPACITY * s.format->stride,
                             BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
    bm->CreateBufferData(pool->IndexBuffer(), BASE_INDEX_CAPACITY * 4,
                         BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);

    // Сначала ВСЕ стрим-инструкции пула, следом его индексная: последняя финализирует цикл
    // дозагрузки (двигает счётчики, чистит стейджинг), а порядок регистрации = порядок исполнения
    // в _ExecuteUpdateInstructions. Обе группы вешает один вызов — переставить их нельзя.
    ModelManager* mm = this;
    for (const GeometryPool::Stream& s : pool->Streams()) {
        const uint32_t src_offset = s.src_offset;
        const uint32_t stride = s.format->stride;
        bm->CreateUpdateInstruction(s.buffer_name,
            [mm, pool, src_offset, stride](SDL_GPUCopyPass*, BufferManager* b, UploadTask& task)
        {
            mm->UploadModelVertexStream(b, &task, pool, src_offset, stride);
        },
            [mm, pool, stride]() -> uint32_t { return mm->CalculateModelsVerticesSize(pool, stride); },
            [mm, pool, stride]() -> uint32_t { return mm->GetVertexBaseOffset(pool, stride); });
    }
    bm->CreateUpdateInstruction(pool->IndexBuffer(),
        [mm, pool](SDL_GPUCopyPass*, BufferManager* b, UploadTask& task)
    {
        mm->UploadModelIndexBuffer(b, &task, pool);
    },
        [mm, pool]() -> uint32_t { return mm->CalculateModelsIndicesSize(pool); },
        [mm, pool]() -> uint32_t { return mm->GetIndexBaseOffset(pool); });

    SDL_Log("Geometry pool '%s' created: %zu streams, %u-byte layout vertex.",
        name.c_str(), pool->Streams().size(), vertex_size);
    return pool;
}

GeometryPool* ModelManager::GetPool(const std::string& name)
{
    if (name.empty()) return default_pool;
    auto it = pools.find(name);
    if (it != pools.end()) return it->second.get();
    SDL_Log("Geometry pool '%s' not found", name.c_str());
    return nullptr;
}

GeometryPool* ModelManager::_ResolvePool(GeometryPool* pool)
{
    if (pool) return pool;
    if (!default_pool)
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ModelManager: no geometry pool created yet.");
    return default_pool;
}

const ModelManager::PoolResidency* ModelManager::_FindResidency(const GeometryPool* pool) const
{
    auto it = residency.find(pool);
    return it != residency.end() ? &it->second : nullptr;
}

// ── Модели ─────────────────────────────────────────────────────────────────────────────────

ModelData* ModelManager::CreateModel(const std::string& name, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor, GeometryPool* pool)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);
    return _LoadModelFile(ptr, _ResolvePool(pool), path_vert, path_ind, anchor);
}

// Upsert: существующий перезагружаем В ТОТ ЖЕ объект (указатель у энтити жив), новый — создаём.
ModelData* ModelManager::LoadModelFromFile(const std::string& name, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor, GeometryPool* pool)
{
    ModelData* ptr;
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        ptr = it->second.get();            // reload в существующий (старая геометрия остаётся в буфере)
    }
    else {
        auto model_data = std::make_unique<ModelData>();
        ptr = model_data.get();
        models_data[name] = std::move(model_data);
    }
    return _LoadModelFile(ptr, _ResolvePool(pool), path_vert, path_ind, anchor);
}

void ModelManager::DeleteModel(const std::string& name)
{
    auto it = models_data.find(name);
    if (it == models_data.end()) return;

    // Место — в отложенный возврат: PendingFree держит КОПИИ Range, поэтому сам объект можно
    // сносить прямо сейчас. Про висячий SubMeshData* в батчах — см. заголовок.
    _ReleaseModelRanges(it->second.get());
    models_data.erase(it);
}

size_t ModelManager::LoadSceneModels(const std::vector<SceneModelEntry>& entries)
{
    size_t loaded = 0;
    for (const SceneModelEntry& e : entries) {
        if (e.name.empty() || e.vertex_path.empty() || e.index_path.empty()) {
            SDL_Log("LoadSceneModels: incomplete entry ('%s') - skipped", e.name.c_str());
            continue;
        }
        // Пул — по имени из манифеста; пусто/промах → дефолтный (сцены без поля не мигрируются).
        GeometryPool* pool = GetPool(e.pool);
        // Замена под тем же именем = снос + создание, как у текстур. Место старой геометрии уходит
        // в отложенный возврат и достаётся новой уже в этом кадре (ReclaimRanges идёт раньше
        // размещения). Битый файл при этом стирает прежнюю геометрию — та же цена, что у текстур.
        if (models_data.count(e.name)) DeleteModel(e.name);
        if (CreateModel(e.name, e.vertex_path, e.index_path, e.anchor, pool)) ++loaded;
    }
    return loaded;
}

ModelData* ModelManager::_LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor)
{
    if (!pool) return ptr;

    // Перезагрузка В ТОТ ЖЕ объект: место старой геометрии возвращаем аллокатору (отложенно —
    // по нему ещё рисуют слоты в полёте). У модели, не дошедшей до заливки, диапазоны пусты,
    // так что двойного возврата при двойной перезагрузке за кадр не будет.
    _ReleaseModelRanges(ptr);

    ptr->model_path = path_vert;   // self-describing: рецепт для редактора/сериализации
    ptr->index_path = path_ind;
    ptr->pool_name = pool->Name();

    PoolResidency& res = _Residency(pool);
    const uint32_t vsize = pool->VertexSize();

    // Жадное чтение: submeshes готовы сразу после возврата (CreateModel всегда на prep-потоке,
    // гонок с общим staging нет). Отложена только заливка на GPU — staging копит данные моделей
    // до батч-апдейтера. Смещения тут СТЕЙДЖИНГ-относительные: куда пачка ляжет в буфере, решит
    // аллокатор при заливке, и он же (через финализатор) переведёт их в абсолютные.
    const uint32_t vbase = safe_u32(res.staging_vertices.size() / vsize);
    const uint32_t ibase = safe_u32(res.staging_indices.size());

    std::vector<SubMeshFileEntry> entries;

    // --- 1. заголовок вершинного файла + размеры (без роста staging) ---
    std::ifstream vf(path_vert, std::ios::binary);
    if (!vf) {
        SDL_Log("CreateModel: failed to open vertex file: %s", path_vert.c_str());
        assert(false && "CreateModel: failed to open vertex file");
        return ptr;
    }
    uint32_t submesh_count = 0;
    vf.read(reinterpret_cast<char*>(&submesh_count), sizeof(uint32_t));
    if (!vf || submesh_count == 0) {
        SDL_Log("CreateModel: failed to read submesh count from: %s", path_vert.c_str());
        assert(false && "CreateModel: bad submesh count");
        return ptr;
    }
    entries.resize(submesh_count);
    vf.read(reinterpret_cast<char*>(entries.data()), submesh_count * sizeof(SubMeshFileEntry));
    if (!vf) {
        SDL_Log("CreateModel: failed to read submesh entries from: %s", path_vert.c_str());
        assert(false && "CreateModel: bad submesh entries");
        return ptr;
    }

    size_t header_size = sizeof(uint32_t) + submesh_count * sizeof(SubMeshFileEntry);
    vf.seekg(0, std::ios::end);
    size_t file_size = vf.tellg();
    size_t vdata_size = file_size - header_size;
    // Размер вершины даёт РАСКЛАДКА пула — файл обязан ей соответствовать.
    if (vdata_size == 0 || vdata_size % vsize != 0) {
        SDL_Log("CreateModel: vertex data size %zu does not match pool '%s' layout (%u B/vertex) in: %s",
            vdata_size, pool->Name().c_str(), vsize, path_vert.c_str());
        assert(false && "CreateModel: invalid vertex data size");
        return ptr;
    }
    uint32_t vcount = safe_u32(vdata_size / vsize);

    // --- 2. размер индексного файла (без роста staging) ---
    std::ifstream indf(path_ind, std::ios::binary);
    if (!indf) {
        SDL_Log("CreateModel: failed to open index file: %s", path_ind.c_str());
        assert(false && "CreateModel: failed to open index file");
        return ptr;
    }
    indf.seekg(0, std::ios::end);
    size_t isize = indf.tellg();
    indf.seekg(0, std::ios::beg);
    if (isize == 0 || isize % sizeof(uint32_t) != 0) {
        SDL_Log("CreateModel: index file empty or invalid: %s", path_ind.c_str());
        assert(false && "CreateModel: invalid index file");
        return ptr;
    }
    uint32_t icount = safe_u32(isize / sizeof(uint32_t));

    // Пивот переписывает позиции — раскладке без записываемой FLOAT3-позиции он недоступен.
    // Это не дыра, а честное свойство раскладки: сообщаем и грузим как Keep.
    if (anchor != AnchorShift::Keep && !pool->HasFloat3Position()) {
        SDL_Log("CreateModel: pool '%s' has no float3 POSITION - anchor shift ignored for '%s'",
            pool->Name().c_str(), path_vert.c_str());
        anchor = AnchorShift::Keep;
    }

    // Все проверки пройдены — растим staging и читаем данные.
    // --- 3. вершины ---
    vf.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
    res.staging_vertices.resize((size_t(vbase) + vcount) * size_t(vsize));
    vf.read(reinterpret_cast<char*>(res.staging_vertices.data() + size_t(vbase) * vsize), vdata_size);
    if (!vf) {
        SDL_Log("CreateModel: incomplete vertex read (%zu / %zu) from %s",
            size_t(vf.gcount()), vdata_size, path_vert.c_str());
        assert(false && "CreateModel: incomplete vertex read");
    }

    // --- 4. индексы ---
    res.staging_indices.resize(size_t(ibase) + icount, 0);
    indf.read(reinterpret_cast<char*>(res.staging_indices.data() + ibase), isize);
    if (!indf) {
        SDL_Log("CreateModel: incomplete index read (%zu / %zu) from %s",
            size_t(indf.gcount()), isize, path_ind.c_str());
        assert(false && "CreateModel: incomplete index read");
    }

    // --- 5. пивот (до сабмешей, чтобы sphere/AABB считались от нового origin) ---
    ptr->anchor = anchor;
    ApplyAnchorShift(res.staging_vertices.data(), pool, vbase, vcount, anchor);

    // --- 6. сабмеши + bounding sphere ---
    BuildSubmeshes(res.staging_vertices.data(), pool, ptr, entries, vbase, ibase);

    _PushBatchEntry(res, ptr, vbase, vcount, ibase, icount);
    res.dirty = true;
    ++spheres_revision;
    return ptr;
}

ModelData* ModelManager::CreateModel(const std::string& name, ModelGeneratorFn generator, AnchorShift anchor, GeometryPool* pool)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    GeometryPool* p = _ResolvePool(pool);
    if (!p) return nullptr;

    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);
    ptr->pool_name = p->Name();

    PoolResidency& res = _Residency(p);
    const uint32_t vsize = p->VertexSize();

    // Жадная генерация: submeshes готовы сразу (как и у дискового пути). Отложена только заливка.
    // Смещения стейджинг-относительные — как и у файлового пути (см. BuildSubmeshes).
    const uint32_t vbase = safe_u32(res.staging_vertices.size() / vsize);
    const uint32_t ibase = safe_u32(res.staging_indices.size());

    std::vector<std::byte> verts;
    std::vector<Uint32>    inds;
    if (generator) generator(verts, inds);
    if (verts.empty() || inds.empty()) {
        SDL_Log("CreateModel: generator produced empty mesh for '%s'", name.c_str());
        assert(false && "CreateModel: empty procedural mesh");
        return ptr;
    }
    // Единственная автоматическая сверка генератора с пулом: байты обязаны делиться на вершину
    // раскладки. Порядок ПОЛЕЙ так не проверить — привязка «структура ↔ пул» остаётся за автором
    // (структура едет вместе со своей *Layout()-функцией), это лишь защита от грубого промаха.
    if (verts.size() % vsize != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "CreateModel('%s'): generator produced %zu bytes, not a multiple of pool '%s' vertex (%u B).",
            name.c_str(), verts.size(), p->Name().c_str(), vsize);
        assert(false && "CreateModel: generated bytes do not fit the pool layout");
        return ptr;
    }
    uint32_t vcount = safe_u32(verts.size() / vsize);
    uint32_t icount = safe_u32(inds.size());

    res.staging_vertices.resize((size_t(vbase) + vcount) * size_t(vsize));
    std::memcpy(res.staging_vertices.data() + size_t(vbase) * vsize, verts.data(), verts.size());
    res.staging_indices.insert(res.staging_indices.end(), inds.begin(), inds.end());

    // Пивот — до сабмешей, чтобы sphere/AABB считались от нового origin.
    ptr->anchor = anchor;
    ApplyAnchorShift(res.staging_vertices.data(), p, vbase, vcount, anchor);

    // Вся геометрия — один сабмеш, материал 0.
    std::vector<SubMeshFileEntry> entries{ SubMeshFileEntry{ 0, 0, vcount, icount, 0 } };
    BuildSubmeshes(res.staging_vertices.data(), p, ptr, entries, vbase, ibase);

    _PushBatchEntry(res, ptr, vbase, vcount, ibase, icount);
    res.dirty = true;
    ++spheres_revision;
    return ptr;
}

// ── Заливка ────────────────────────────────────────────────────────────────────────────────

uint32_t ModelManager::CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride)
{
    // Чистый геттер: staging заполнен жадно в CreateModel. Размер — на ОДИН стрим пула.
    const PoolResidency* res = _FindResidency(pool);
    if (!res || !res->dirty) return 0;
    return safe_u32(res->staging_vertices.size() / pool->VertexSize() * stream_stride);
}

uint32_t ModelManager::CalculateModelsIndicesSize(const GeometryPool* pool)
{
    const PoolResidency* res = _FindResidency(pool);
    if (!res || !res->dirty) return 0;
    return safe_u32(res->staging_indices.size() * sizeof(Uint32));
}

uint32_t ModelManager::GetVertexBaseOffset(const GeometryPool* pool, uint32_t stream_stride)
{
    _EnsureBatchAllocation(pool);
    const PoolResidency* res = _FindResidency(pool);
    // Элементы → байты СВОЕГО стрима: стримы растут в ногу, поэтому один элементный диапазон
    // обслуживает все три, каждый по своему страйду.
    return (res && res->batch_allocated) ? res->batch_verts.first * stream_stride : 0;
}

uint32_t ModelManager::GetIndexBaseOffset(const GeometryPool* pool)
{
    _EnsureBatchAllocation(pool);
    const PoolResidency* res = _FindResidency(pool);
    // Аллокатор индексов считает В ЭЛЕМЕНТАХ (как first_index команды), а заливке нужны БАЙТЫ.
    return (res && res->batch_allocated) ? res->batch_index.first * safe_u32(sizeof(Uint32)) : 0;
}

void ModelManager::_EnsureBatchAllocation(const GeometryPool* pool)
{
    PoolResidency& res = _Residency(pool);
    if (res.batch_allocated || !res.dirty || res.staging_vertices.empty()) return;

    // Одно выделение на всю пачку: инструкция заливки пишет один непрерывный диапазон. Не нашлось
    // подходящей дыры — аллокатор отдаст место с вершины, то есть ровно прежнее поведение
    // «дозаписать в конец». Хуже, чем было, не станет никогда.
    res.batch_verts = res.verts.Allocate(safe_u32(res.staging_vertices.size() / pool->VertexSize()));
    res.batch_index = res.index.Allocate(safe_u32(res.staging_indices.size()));
    res.batch_allocated = true;
}

void ModelManager::_PushBatchEntry(PoolResidency& res, ModelData* model, uint32_t vbase, uint32_t vcount,
    uint32_t ibase, uint32_t icount)
{
    // Без дубля: одно имя могли перезагрузить дважды за кадр — тогда актуальна последняя запись
    // (её сабмеши и лежат в модели), а первая стала мусором в стейджинге.
    for (BatchEntry& e : res.batch)
        if (e.model == model) { e = { model, vbase, vcount, ibase, icount }; return; }
    res.batch.push_back({ model, vbase, vcount, ibase, icount });
}

void ModelManager::_ReleaseModelRanges(ModelData* model)
{
    if (!model || (model->vertex_range.count == 0 && model->index_range.count == 0)) return;
    auto pit = pools.find(model->pool_name);
    if (pit == pools.end()) return;   // пул не резолвится — место вернуть некуда

    _Residency(pit->second.get()).pending_free.push_back({ model->vertex_range, model->index_range });
    model->vertex_range = {};
    model->index_range = {};
}

void ModelManager::ReclaimRanges()
{
    // Массовое освобождение перед массовым размещением — та же двухфазность, что у атласов
    // (PackAtlases: сначала _ReleasePendingRegions, потом раскладка новых). Обоснование отсутствия
    // фенс-штампа — в заголовке.
    for (auto& [pool, res] : residency) {
        for (const PendingFree& p : res.pending_free) {
            res.verts.Free(p.verts);
            res.index.Free(p.index);
        }
        res.pending_free.clear();
    }
}

void ModelManager::UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
    uint32_t src_offset, uint32_t stream_stride)
{
    PoolResidency& res = _Residency(pool);
    if (!res.dirty || res.staging_vertices.empty()) return;

    // Стейджинг — интерлив (формат ЗАГРУЗКИ пула); стрим — плотный срез каждой вершины.
    // Гатер прямо в mapped transfer-буфер (write-combined: только писать), без промежуточной копии.
    const uint32_t vsize = pool->VertexSize();
    const size_t   count = res.staging_vertices.size() / vsize;
    const uint32_t bytes = safe_u32(count * stream_stride);
    std::byte* dst = static_cast<std::byte*>(bm->AcquireTransferWritePtr(task, bytes));
    if (!dst) return;
    const std::byte* src = res.staging_vertices.data() + src_offset;
    for (size_t i = 0; i < count; ++i)
        SDL_memcpy(dst + i * stream_stride, src + i * vsize, stream_stride);
}

void ModelManager::UploadModelIndexBuffer(BufferManager* bm, UploadTask* task, const GeometryPool* pool)
{
    PoolResidency& res = _Residency(pool);
    if (!res.dirty || res.staging_indices.empty()) return;

    uint32_t ibytes = safe_u32(res.staging_indices.size() * sizeof(Uint32));
    bm->UploadToTransferBuffer(task, ibytes, res.staging_indices.data());

    // Индексный апдейтер идёт последним (после ВСЕХ стрим-заливок ЭТОГО пула) — финализируем цикл.
    // База пачки теперь известна И залита, поэтому здесь и только здесь смещения сабмешей
    // переводятся из стейджинг-относительных в абсолютные. Батч-дерево держит УКАЗАТЕЛИ на
    // SubMeshData, так что правка на месте видна уже собранным батчам; а индиректы читают
    // submesh->indexOffset в своём апдейтере, зарегистрированном ПОЗЖЕ этого — значит порядок фаз
    // сам гарантирует, что они увидят уже абсолютные.
    _EnsureBatchAllocation(pool);
    for (const BatchEntry& e : res.batch) {
        if (!e.model) continue;
        for (SubMeshData& s : e.model->submeshes) {
            s.vertexOffset += res.batch_verts.first;
            s.indexOffset += res.batch_index.first;
        }
        // Место модели — чтобы вернуть его аллокатору при следующей перезагрузке.
        e.model->vertex_range = { res.batch_verts.first + e.vbase, e.vcount };
        e.model->index_range = { res.batch_index.first + e.ibase, e.icount };
    }
    res.batch.clear();
    res.batch_allocated = false;
    res.batch_verts = {};
    res.batch_index = {};

    res.staging_vertices.clear();
    res.staging_vertices.shrink_to_fit();
    res.staging_indices.clear();
    res.staging_indices.shrink_to_fit();

    res.dirty = false;
}

bool ModelManager::CheckDirty() const
{
    for (const auto& [pool, res] : residency)
        if (res.dirty) return true;
    return false;
}

ModelData* ModelManager::operator[](const std::string& name)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        return it->second.get();
    }
    SDL_Log("Model '%s' not found", name.c_str());
    return nullptr;
}

ModelManager::~ModelManager()
{
    models_data.clear();
    residency.clear();
    pools.clear();
}
