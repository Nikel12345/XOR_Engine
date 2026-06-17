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
            glm::vec3 center(0.0f);
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                const PosUVNormal& v = staging[vbase + e.vertexOffset + k];
                center += glm::vec3(v.x, v.y, v.z);
            }
            center /= static_cast<float>(e.vertexCount);

            float radius = 0.0f;
            for (uint32_t k = 0; k < e.vertexCount; ++k) {
                const PosUVNormal& v = staging[vbase + e.vertexOffset + k];
                float d = glm::distance(center, glm::vec3(v.x, v.y, v.z));
                if (d > radius) radius = d;
            }
            sub.sphere = glm::vec4(center, radius);
        }
        model->submeshes.push_back(sub);
    }
}

ModelData* ModelManager::CreateModel(const std::string& name, const std::string& path_vert, const std::string& path_ind)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    // Никакого чтения с диска: только регистрируем модель. Заголовок, данные, сабмеши,
    // смещения и сферы — всё в LoadModels (prep-фаза). Возвращаем пока пустой ModelData:
    // submeshes заполнятся до батч-билдера, который их единственный и читает.
    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);

    PendingModel pm;
    pm.model = ptr;
    pm.source = PendingModel::FileSource{ path_vert, path_ind };
    pending_models.push_back(std::move(pm));
    dirty = true;
    dirty_spheres = true;
    return ptr;
}

ModelData* ModelManager::CreateModel(const std::string& name, ModelGeneratorFn generator)
{
    auto it = models_data.find(name);
    if (it != models_data.end()) {
        SDL_Log("Model '%s' already exists, returning existing model data.", name.c_str());
        return it->second.get();
    }

    // Никакой генерации здесь: только регистрируем. generator вызовется в LoadModels.
    auto model_data = std::make_unique<ModelData>();
    ModelData* ptr = model_data.get();
    models_data[name] = std::move(model_data);

    PendingModel pm;
    pm.model = ptr;
    pm.source = std::move(generator);
    pending_models.push_back(std::move(pm));
    dirty = true;
    dirty_spheres = true;
    return ptr;
}

void ModelManager::LoadModels()
{
    if (!dirty) return;

    staging_vertices.clear();
    staging_indices.clear();

    // Курсоры-смещения идут от уже залитого на GPU объёма. Постоянные счётчики
    // (total_*) двигаются только при финализации после реальной заливки — поэтому
    // повторный LoadModels без финализации идемпотентен (пересчитает те же смещения).
    uint32_t voff = total_vertices_count;
    uint32_t ioff = total_indices_count;

    for (const PendingModel& pm : pending_models) {
        std::vector<SubMeshFileEntry> entries;
        size_t   vbase = staging_vertices.size();
        uint32_t vcount = 0;
        uint32_t icount = 0;

        if (auto* gen = std::get_if<ModelGeneratorFn>(&pm.source)) {
            // --- Процедурная модель: генератор сам выдаёт вершины/индексы ---
            std::vector<PosUVNormal> verts;
            std::vector<Uint32>      inds;
            (*gen)(verts, inds);
            if (verts.empty() || inds.empty()) {
                SDL_Log("LoadModels: generator produced empty mesh");
                assert(false && "LoadModels: empty procedural mesh");
                continue;
            }
            vcount = safe_u32(verts.size());
            icount = safe_u32(inds.size());
            staging_vertices.insert(staging_vertices.end(), verts.begin(), verts.end());
            staging_indices.insert(staging_indices.end(), inds.begin(), inds.end());
            // Вся геометрия — один сабмеш, материал 0.
            entries.push_back(SubMeshFileEntry{ 0, 0, vcount, icount, 0 });
        }
        else {
            const PendingModel::FileSource& fs = std::get<PendingModel::FileSource>(pm.source);

            // --- 1. заголовок вершинного файла + размеры (без роста staging) ---
            std::ifstream vf(fs.vert_path, std::ios::binary);
            if (!vf) {
                SDL_Log("LoadModels: failed to open vertex file: %s", fs.vert_path.c_str());
                assert(false && "LoadModels: failed to open vertex file");
                continue;
            }
            uint32_t submesh_count = 0;
            vf.read(reinterpret_cast<char*>(&submesh_count), sizeof(uint32_t));
            if (!vf || submesh_count == 0) {
                SDL_Log("LoadModels: failed to read submesh count from: %s", fs.vert_path.c_str());
                assert(false && "LoadModels: bad submesh count");
                continue;
            }
            entries.resize(submesh_count);
            vf.read(reinterpret_cast<char*>(entries.data()), submesh_count * sizeof(SubMeshFileEntry));
            if (!vf) {
                SDL_Log("LoadModels: failed to read submesh entries from: %s", fs.vert_path.c_str());
                assert(false && "LoadModels: bad submesh entries");
                continue;
            }

            size_t header_size = sizeof(uint32_t) + submesh_count * sizeof(SubMeshFileEntry);
            vf.seekg(0, std::ios::end);
            size_t file_size = vf.tellg();
            size_t vdata_size = file_size - header_size;
            if (vdata_size == 0 || vdata_size % sizeof(PosUVNormal) != 0) {
                SDL_Log("LoadModels: invalid vertex data size in: %s", fs.vert_path.c_str());
                assert(false && "LoadModels: invalid vertex data size");
                continue;
            }
            vcount = safe_u32(vdata_size / sizeof(PosUVNormal));

            // --- 2. размер индексного файла (без роста staging) ---
            std::ifstream indf(fs.ind_path, std::ios::binary);
            if (!indf) {
                SDL_Log("LoadModels: failed to open index file: %s", fs.ind_path.c_str());
                assert(false && "LoadModels: failed to open index file");
                continue;
            }
            indf.seekg(0, std::ios::end);
            size_t isize = indf.tellg();
            indf.seekg(0, std::ios::beg);
            if (isize == 0 || isize % sizeof(uint32_t) != 0) {
                SDL_Log("LoadModels: index file empty or invalid: %s", fs.ind_path.c_str());
                assert(false && "LoadModels: invalid index file");
                continue;
            }
            icount = safe_u32(isize / sizeof(uint32_t));

            // Все проверки пройдены — теперь растим staging и читаем данные.
            // --- 3. вершины ---
            vf.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
            staging_vertices.resize(vbase + vcount, PosUVNormal{});
            vf.read(reinterpret_cast<char*>(staging_vertices.data() + vbase), vdata_size);
            if (!vf) {
                SDL_Log("LoadModels: incomplete vertex read (%zu / %zu) from %s",
                    size_t(vf.gcount()), vdata_size, fs.vert_path.c_str());
                assert(false && "LoadModels: incomplete vertex read");
            }

            // --- 4. индексы ---
            size_t ibase = staging_indices.size();
            staging_indices.resize(ibase + icount, 0);
            indf.read(reinterpret_cast<char*>(staging_indices.data() + ibase), isize);
            if (!indf) {
                SDL_Log("LoadModels: incomplete index read (%zu / %zu) from %s",
                    size_t(indf.gcount()), isize, fs.ind_path.c_str());
                assert(false && "LoadModels: incomplete index read");
            }
        }

        // --- сабмеши + bounding sphere (общее для диска и генератора) ---
        BuildSubmeshes(staging_vertices, pm.model, entries, vbase, voff, ioff);

        voff += vcount;
        ioff += icount;
    }
}

uint32_t ModelManager::CalculateModelsVerticesSize()
{
    // Чистый геттер: staging уже заполнен LoadModels в общей части prep-фазы.
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
    // Счётчики двигаем только здесь, после реальной заливки (см. LoadModels: курсоры
    // идут от total_*, поэтому повторный LoadModels без финализации идемпотентен).
    total_vertices_count += safe_u32(staging_vertices.size());
    total_indices_count += safe_u32(staging_indices.size());
    gpu_vertices_bytes += safe_u32(staging_vertices.size() * sizeof(PosUVNormal));
    gpu_indices_bytes += ibytes;

    staging_vertices.clear();
    staging_vertices.shrink_to_fit();
    staging_indices.clear();
    staging_indices.shrink_to_fit();
    pending_models.clear();

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
    pending_models.clear();
    staging_vertices.clear();
    staging_indices.clear();
}
