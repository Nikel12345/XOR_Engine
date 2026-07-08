#include "PCH.h"
#include <fstream>
#include <iostream>
#include "ModelManager.h"
#include "BufferManager.h"

ModelManager::ModelManager() {};

struct SubMeshFileEntry {
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t material_index;
};

// Строит сабмеши модели из записей entries: глобальные смещения = курсоры + локальное
// смещение записи, плюс bounding sphere по вершинам из staging. Общее для диска и генератора.
static void BuildSubmeshes(const std::vector<PosUVNormal>& staging, ModelData* model,
    const std::vector<SubMeshFileEntry>& entries, size_t vbase, uint32_t voff, uint32_t ioff)
{
    model->submeshes.clear();
    model->submeshes.reserve(entries.size());
    for (const SubMeshFileEntry& e : entries) {
        SubMeshData sub{};
        sub.vertexOffset = voff + e.vertexOffset;
        sub.indexOffset = ioff + e.indexOffset;
        sub.vertexCount = e.vertexCount;
        sub.indexCount = e.indexCount;
        sub.material_index = e.material_index;
        sub.sphere = glm::vec4(0.0f);

        if (e.vertexCount > 0) {
            const PosUVNormal& v0 = staging[vbase + e.vertexOffset];
            glm::vec3 center(0.0f);
            glm::vec3 mn(v0.x, v0.y, v0.z), mx(v0.x, v0.y, v0.z);
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                const PosUVNormal& v = staging[vbase + e.vertexOffset + k];
                glm::vec3 p(v.x, v.y, v.z);
                center += p;
                mn = glm::min(mn, p);
                mx = glm::max(mx, p);
            }
            center /= static_cast<float>(e.vertexCount);

            float radius = 0.0f;
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                const PosUVNormal& v = staging[vbase + e.vertexOffset + k];
                float d = glm::distance(center, glm::vec3(v.x, v.y, v.z));
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
static void ApplyAnchorShift(std::vector<PosUVNormal>& staging, size_t vbase, uint32_t vcount, AnchorShift anchor)
{
    if (anchor == AnchorShift::Keep || vcount == 0) return;

    glm::vec3 mn(staging[vbase].x, staging[vbase].y, staging[vbase].z), mx = mn;
    for (uint32_t k = 1; k < vcount; ++k) {
        const PosUVNormal& v = staging[vbase + k];
        mn = glm::min(mn, glm::vec3(v.x, v.y, v.z));
        mx = glm::max(mx, glm::vec3(v.x, v.y, v.z));
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

    for (uint32_t k = 0; k < vcount; ++k) {
        PosUVNormal& v = staging[vbase + k];
        v.x -= q.x; v.y -= q.y; v.z -= q.z;
    }
}

ModelData* ModelManager::CreateModel(const std::string& name, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);
    return _LoadModelFile(ptr, path_vert, path_ind, anchor);
}

// Upsert: существующий перезагружаем В ТОТ ЖЕ объект (указатель у энтити жив), новый — создаём.
ModelData* ModelManager::LoadModelFromFile(const std::string& name, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor)
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
    return _LoadModelFile(ptr, path_vert, path_ind, anchor);
}

ModelData* ModelManager::_LoadModelFile(ModelData* ptr, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor)
{
    ptr->model_path = path_vert;   // self-describing: рецепт для редактора/сериализации
    ptr->index_path = path_ind;

    // Жадное чтение: submeshes готовы сразу после возврата (CreateModel всегда на prep-потоке,
    // гонок с общим staging нет). Отложена только заливка на GPU — staging копит данные
    // моделей до батч-апдейтера, который дозаписывает их в конец GPU-буфера.
    // Курсоры глобальных смещений = уже залито (total_*) + уже в staging (ещё не залито).
    const size_t   vbase = staging_vertices.size();
    const uint32_t voff = total_vertices_count + safe_u32(staging_vertices.size());
    const uint32_t ioff = total_indices_count + safe_u32(staging_indices.size());

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
    if (vdata_size == 0 || vdata_size % sizeof(PosUVNormal) != 0) {
        SDL_Log("CreateModel: invalid vertex data size in: %s", path_vert.c_str());
        assert(false && "CreateModel: invalid vertex data size");
        return ptr;
    }
    uint32_t vcount = safe_u32(vdata_size / sizeof(PosUVNormal));

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

    // Все проверки пройдены — растим staging и читаем данные.
    // --- 3. вершины ---
    vf.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
    staging_vertices.resize(vbase + vcount, PosUVNormal{});
    vf.read(reinterpret_cast<char*>(staging_vertices.data() + vbase), vdata_size);
    if (!vf) {
        SDL_Log("CreateModel: incomplete vertex read (%zu / %zu) from %s",
            size_t(vf.gcount()), vdata_size, path_vert.c_str());
        assert(false && "CreateModel: incomplete vertex read");
    }

    // --- 4. индексы ---
    size_t ibase = staging_indices.size();
    staging_indices.resize(ibase + icount, 0);
    indf.read(reinterpret_cast<char*>(staging_indices.data() + ibase), isize);
    if (!indf) {
        SDL_Log("CreateModel: incomplete index read (%zu / %zu) from %s",
            size_t(indf.gcount()), isize, path_ind.c_str());
        assert(false && "CreateModel: incomplete index read");
    }

    // --- 5. пивот (до сабмешей, чтобы sphere/AABB считались от нового origin) ---
    ptr->anchor = anchor;
    ApplyAnchorShift(staging_vertices, vbase, vcount, anchor);

    // --- 6. сабмеши + bounding sphere ---
    BuildSubmeshes(staging_vertices, ptr, entries, vbase, voff, ioff);

    dirty = true;
    dirty_spheres = true;
    return ptr;
}

ModelData* ModelManager::CreateModel(const std::string& name, ModelGeneratorFn generator, AnchorShift anchor)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);

    // Жадная генерация: submeshes готовы сразу (как и у дискового пути). Отложена только заливка.
    const size_t   vbase = staging_vertices.size();
    const uint32_t voff = total_vertices_count + safe_u32(staging_vertices.size());
    const uint32_t ioff = total_indices_count + safe_u32(staging_indices.size());

    std::vector<PosUVNormal> verts;
    std::vector<Uint32>      inds;
    if (generator) generator(verts, inds);
    if (verts.empty() || inds.empty()) {
        SDL_Log("CreateModel: generator produced empty mesh for '%s'", name.c_str());
        assert(false && "CreateModel: empty procedural mesh");
        return ptr;
    }
    uint32_t vcount = safe_u32(verts.size());
    uint32_t icount = safe_u32(inds.size());
    staging_vertices.insert(staging_vertices.end(), verts.begin(), verts.end());
    staging_indices.insert(staging_indices.end(), inds.begin(), inds.end());

    // Пивот — до сабмешей, чтобы sphere/AABB считались от нового origin.
    ptr->anchor = anchor;
    ApplyAnchorShift(staging_vertices, vbase, vcount, anchor);

    // Вся геометрия — один сабмеш, материал 0.
    std::vector<SubMeshFileEntry> entries{ SubMeshFileEntry{ 0, 0, vcount, icount, 0 } };
    BuildSubmeshes(staging_vertices, ptr, entries, vbase, voff, ioff);

    dirty = true;
    dirty_spheres = true;
    return ptr;
}

uint32_t ModelManager::CalculateModelsVerticesSize()
{
    // Чистый геттер: staging заполнен жадно в CreateModel.
    if (!dirty)
        return 0;
    return safe_u32(staging_vertices.size() * sizeof(PosUVNormal));
}

uint32_t ModelManager::CalculateModelsIndicesSize()
{
    if (!dirty)
        return 0;
    return safe_u32(staging_indices.size() * sizeof(Uint32));
}

void ModelManager::UploadModelVertexBuffer(BufferManager* bm, UploadTask* task)
{
    if (!dirty || staging_vertices.empty()) return;
    uint32_t bytes = safe_u32(staging_vertices.size() * sizeof(PosUVNormal));
    bm->UploadToTransferBuffer(task, bytes, staging_vertices.data());
}

void ModelManager::UploadModelIndexBuffer(BufferManager* bm, UploadTask* task)
{
    if (!dirty || staging_indices.empty()) return;
    uint32_t ibytes = safe_u32(staging_indices.size() * sizeof(Uint32));
    bm->UploadToTransferBuffer(task, ibytes, staging_indices.data());

    // Индексный апдейтер идёт последним — финализируем цикл дозагрузки.
    // Счётчики двигаем только здесь, после реальной заливки: пока модели лежат в staging,
    // CreateModel считает их смещения от total_* + размера staging (см. курсоры там).
    total_vertices_count += safe_u32(staging_vertices.size());
    total_indices_count += safe_u32(staging_indices.size());
    gpu_vertices_bytes += safe_u32(staging_vertices.size() * sizeof(PosUVNormal));
    gpu_indices_bytes += ibytes;

    staging_vertices.clear();
    staging_vertices.shrink_to_fit();
    staging_indices.clear();
    staging_indices.shrink_to_fit();

    dirty = false;
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
    staging_vertices.clear();
    staging_indices.clear();
}
