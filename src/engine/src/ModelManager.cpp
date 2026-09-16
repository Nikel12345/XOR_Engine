#include "PCH.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include "ModelManager.h"
#include <algorithm>
#include "BufferManager.h"

ModelManager::ModelManager() {};

class RangeAllocator
{
public:
    GeometryRange Allocate(uint32_t count)
    {
        if (count == 0) return {};

        for (size_t i = 0; i < free_.size(); ++i) {
            if (free_[i].count < count) continue;
            GeometryRange out{ free_[i].first, count };
            if (free_[i].count == count) free_.erase(free_.begin() + i);
            else { free_[i].first += count; free_[i].count -= count; }
            return out;
        }

        GeometryRange out{ top_, count };
        top_ += count;
        return out;
    }

    void Free(GeometryRange r)
    {
        if (r.count == 0) return;

        auto it = std::lower_bound(free_.begin(), free_.end(), r.first,
            [](const GeometryRange& a, uint32_t f) { return a.first < f; });
        it = free_.insert(it, r);

        // Сначала правый сосед, потом левый: после слияния с правым блок вырастает, и левый должен
        // видеть уже итоговую границу — иначе цепочка из трёх смежных дыр слипнется не полностью.
        if (it + 1 != free_.end() && it->first + it->count == (it + 1)->first) {
            it->count += (it + 1)->count;
            free_.erase(it + 1);
        }
        if (it != free_.begin() && (it - 1)->first + (it - 1)->count == it->first) {
            (it - 1)->count += it->count;
            free_.erase(it);
        }

        // Дыра, дошедшая до вершины, дырой не хранится: вершина опускается, и освободившийся хвост
        // буфера снова считается свободным местом.
        if (!free_.empty() && free_.back().first + free_.back().count == top_) {
            top_ = free_.back().first;
            free_.pop_back();
        }
    }

private:
    std::vector<GeometryRange> free_;
    uint32_t top_ = 0;
};


struct PoolResidency {
    // БАЙТЫ раскладки пула: вершина i начинается с i * VertexSize().
    std::vector<std::byte> staging_vertices;
    std::vector<Uint32>    staging_indices;
    bool dirty = false;

    RangeAllocator verts;
    RangeAllocator index;

    GeometryRange batch_verts;
    GeometryRange batch_index;
    bool batch_allocated = false;
    std::vector<ModelData*> batch;

    std::vector<std::pair<GeometryRange, GeometryRange>> pending_free;
};

static constexpr uint32_t BASE_VERTEX_COUNT = 1024;   // вершин на стрим, байты = count * stride
static constexpr uint32_t BASE_INDEX_COUNT = 4096;    // индексов uint32

struct SubMeshFileEntry {
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t material_index;
};

// memcpy, а не разыменование: раскладка приходит извне, и выравнивание позиции в её вершине
// ничем не гарантировано.
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

// Раскладка без читаемой позиции законна: сабмеши получают вырожденную сферу w = -1, и отсев
// трактует такую модель как видимую всегда.
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
            sub.aabb_center = (mn + mx) * 0.5f;
            sub.aabb_half = (mx - mn) * 0.5f;
        }
        model->submeshes.push_back(sub);
    }
}

// Вызывается ДО BuildSubmeshes: тогда сфера и AABB сабмешей считаются уже от нового origin.
// Раскладку без записываемой FLOAT3-позиции вызывающий обязан отсеять раньше.
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
    if (pool->Streams().empty()) {
        return nullptr;
    }
    pools[name] = std::move(owned);
    _Residency(pool);
    if (!default_pool) default_pool = pool;

    for (const GeometryPool::Stream& s : pool->Streams())
        bm->CreateBufferData(s.buffer_name, BASE_VERTEX_COUNT * s.format->stride,
                             BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
    bm->CreateBufferData(pool->IndexBuffer(), BASE_INDEX_COUNT * safe_u32(sizeof(Uint32)),
                         BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);

    // Порядок регистрации = порядок исполнения, поэтому индексная инструкция идёт последней и
    // закрывает цикл дозагрузки.
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

PoolResidency& ModelManager::_Residency(const GeometryPool* pool)
{
    std::unique_ptr<PoolResidency>& slot = residency[pool];
    if (!slot) slot = std::make_unique<PoolResidency>();
    return *slot;
}

const PoolResidency* ModelManager::_FindResidency(const GeometryPool* pool) const
{
    auto it = residency.find(pool);
    return it != residency.end() ? it->second.get() : nullptr;
}


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

// Перезагрузка идёт В ТОТ ЖЕ объект: сырой указатель на модель может быть у кода игры.
ModelData* ModelManager::LoadModelFromFile(const std::string& name, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor, GeometryPool* pool)
{
    ModelData* ptr;
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        ptr = it->second.get();   // старая геометрия остаётся в буфере
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

    _ReleaseModelRanges(it->second.get());
    models_data.erase(it);
}

void ModelManager::SetSubmeshSpan(const std::string& name, size_t submesh, SubMeshSpan span)
{
    ModelData* m = FindModel(name);
    if (!m || submesh >= m->submeshes.size()) return;
    SubMeshSpan& dst = m->submeshes[submesh].screen_size_span;
    if (dst.lod_min == span.lod_min && dst.lod_max == span.lod_max) return;
    dst = span;
}

size_t ModelManager::ClearSceneModels()
{
    std::vector<std::string> doomed;
    for (const auto& [name, m] : models_data)
        if (!m || (!HasTag(m->tags, ResourceTag::CodeOwned) && !m->model_path.empty()))
            doomed.push_back(name);
    for (const std::string& n : doomed) DeleteModel(n);
    return doomed.size();
}

size_t ModelManager::LoadSceneModels(const std::vector<SceneModelEntry>& entries)
{
    size_t loaded = 0;
    for (const SceneModelEntry& e : entries) {
        if (e.name.empty() || e.vertex_path.empty() || e.index_path.empty()) {
            SDL_Log("LoadSceneModels: incomplete entry ('%s') - skipped", e.name.c_str());
            continue;
        }
        GeometryPool* pool = GetPool(e.pool);
        // Битый файл стирает прежнюю геометрию: замена под тем же именем — это снос и создание.
        if (models_data.count(e.name)) DeleteModel(e.name);
        if (!CreateModel(e.name, e.vertex_path, e.index_path, e.anchor, pool)) continue;
        ++loaded;
        // Диапазоны приходят из манифеста, а не из .bin, и в построении геометрии не участвуют.
        ModelData* md = models_data.at(e.name).get();
        const size_t n = std::min(e.screen_size_span.size(), md->submeshes.size());
        for (size_t i = 0; i < n; ++i) md->submeshes[i].screen_size_span = e.screen_size_span[i];
    }
    return loaded;
}

ModelData* ModelManager::_LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor)
{
    if (!pool) return ptr;

    // У модели, не дошедшей до заливки, диапазоны пусты — двойного возврата при двойной
    // перезагрузке за кадр не будет.
    _ReleaseModelRanges(ptr);

    ptr->model_path = path_vert;
    ptr->index_path = path_ind;
    ptr->pool_name = pool->Name();

    PoolResidency& res = _Residency(pool);
    const uint32_t vsize = pool->VertexSize();

    const uint32_t vbase = safe_u32(res.staging_vertices.size() / vsize);
    const uint32_t ibase = safe_u32(res.staging_indices.size());

    std::vector<SubMeshFileEntry> entries;

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
    if (vdata_size == 0 || vdata_size % vsize != 0) {
        SDL_Log("CreateModel: vertex data size %zu does not match pool '%s' layout (%u B/vertex) in: %s",
            vdata_size, pool->Name().c_str(), vsize, path_vert.c_str());
        assert(false && "CreateModel: invalid vertex data size");
        return ptr;
    }
    uint32_t vcount = safe_u32(vdata_size / vsize);

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

    // Пивот переписывает позиции, поэтому раскладке без записываемой FLOAT3-позиции он
    // недоступен: сообщаем и грузим как Keep.
    if (anchor != AnchorShift::Keep && !pool->HasFloat3Position()) {
        SDL_Log("CreateModel: pool '%s' has no float3 POSITION - anchor shift ignored for '%s'",
            pool->Name().c_str(), path_vert.c_str());
        anchor = AnchorShift::Keep;
    }

    vf.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
    res.staging_vertices.resize((size_t(vbase) + vcount) * size_t(vsize));
    vf.read(reinterpret_cast<char*>(res.staging_vertices.data() + size_t(vbase) * vsize), vdata_size);
    if (!vf) {
        SDL_Log("CreateModel: incomplete vertex read (%zu / %zu) from %s",
            size_t(vf.gcount()), vdata_size, path_vert.c_str());
        assert(false && "CreateModel: incomplete vertex read");
    }

    res.staging_indices.resize(size_t(ibase) + icount, 0);
    indf.read(reinterpret_cast<char*>(res.staging_indices.data() + ibase), isize);
    if (!indf) {
        SDL_Log("CreateModel: incomplete index read (%zu / %zu) from %s",
            size_t(indf.gcount()), isize, path_ind.c_str());
        assert(false && "CreateModel: incomplete index read");
    }

    ptr->anchor = anchor;
    ApplyAnchorShift(res.staging_vertices.data(), pool, vbase, vcount, anchor);

    BuildSubmeshes(res.staging_vertices.data(), pool, ptr, entries, vbase, ibase);

    _PushBatchEntry(res, ptr, { vbase, vcount }, { ibase, icount });
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
    // Единственная автоматическая сверка генератора с пулом: порядок ПОЛЕЙ так не проверить,
    // привязка «структура — пул» остаётся за автором.
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

    ptr->anchor = anchor;
    ApplyAnchorShift(res.staging_vertices.data(), p, vbase, vcount, anchor);

    std::vector<SubMeshFileEntry> entries{ SubMeshFileEntry{ 0, 0, vcount, icount, 0 } };
    BuildSubmeshes(res.staging_vertices.data(), p, ptr, entries, vbase, ibase);

    _PushBatchEntry(res, ptr, { vbase, vcount }, { ibase, icount });
    res.dirty = true;
    ++spheres_revision;
    return ptr;
}


uint32_t ModelManager::CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride)
{
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
    return (res && res->batch_allocated) ? res->batch_verts.first * stream_stride : 0;
}

uint32_t ModelManager::GetIndexBaseOffset(const GeometryPool* pool)
{
    _EnsureBatchAllocation(pool);
    const PoolResidency* res = _FindResidency(pool);
    // Аллокатор считает В ЭЛЕМЕНТАХ, а заливке нужны БАЙТЫ.
    return (res && res->batch_allocated) ? res->batch_index.first * safe_u32(sizeof(Uint32)) : 0;
}

void ModelManager::PackModels()
{
    for (auto& [pool, slot] : residency) {
        PoolResidency& res = *slot;
        _EnsureBatchAllocation(pool);
        if (!res.batch_allocated) continue;

        for (ModelData* model : res.batch) {
            if (!model || model->placed) continue;
            for (SubMeshData& s : model->submeshes) {
                s.vertexOffset += res.batch_verts.first;
                s.indexOffset += res.batch_index.first;
            }
            model->vertex_range.first += res.batch_verts.first;
            model->index_range.first += res.batch_index.first;
            model->placed = true;
        }
    }
}

void ModelManager::_EnsureBatchAllocation(const GeometryPool* pool)
{
    PoolResidency& res = _Residency(pool);
    if (res.batch_allocated || !res.dirty || res.staging_vertices.empty()) return;

    res.batch_verts = res.verts.Allocate(safe_u32(res.staging_vertices.size() / pool->VertexSize()));
    res.batch_index = res.index.Allocate(safe_u32(res.staging_indices.size()));
    res.batch_allocated = true;
}

void ModelManager::_PushBatchEntry(PoolResidency& res, ModelData* model,
    GeometryRange verts, GeometryRange index)
{
    model->vertex_range = verts;   // стейджинг-относительные до PackModels
    model->index_range = index;
    model->placed = false;

    // Одно имя могли перезагрузить дважды за кадр: первая запись стала мусором в стейджинге.
    for (ModelData* m : res.batch)
        if (m == model) return;
    res.batch.push_back(model);
}

void ModelManager::_ReleaseModelRanges(ModelData* model)
{
    if (!model || !model->placed) return;
    auto pit = pools.find(model->pool_name);
    if (pit == pools.end()) return;   // пул не резолвится — место вернуть некуда

    _Residency(pit->second.get()).pending_free.push_back({ model->vertex_range, model->index_range });
    model->vertex_range = {};
    model->index_range = {};
    model->placed = false;
}

void ModelManager::ReclaimRanges()
{
    for (auto& [pool, slot] : residency) {
        PoolResidency& res = *slot;
        for (const auto& [verts, index] : res.pending_free) {
            res.verts.Free(verts);
            res.index.Free(index);
        }
        res.pending_free.clear();
    }
}

void ModelManager::UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
    uint32_t src_offset, uint32_t stream_stride)
{
    PoolResidency& res = _Residency(pool);
    if (!res.dirty || res.staging_vertices.empty()) return;

    // Гатер прямо в mapped transfer-буфер: он write-combined, читать из него нельзя.
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

    // Идёт после ВСЕХ стрим-заливок этого пула — закрываем цикл. Размещение уже сделал PackModels.
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
    for (const auto& [pool, slot] : residency)
        if (slot->dirty) return true;
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
