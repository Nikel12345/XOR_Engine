#include "PCH.h"
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

    pending_models.push_back(PendingModel{ ptr, path_vert, path_ind });
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
        // --- 1. заголовок вершинного файла + размеры (без роста staging) ---
        std::ifstream vf(pm.vert_path, std::ios::binary);
        if (!vf) {
            SDL_Log("LoadModels: failed to open vertex file: %s", pm.vert_path.c_str());
            assert(false && "LoadModels: failed to open vertex file");
            continue;
        }
        uint32_t submesh_count = 0;
        vf.read(reinterpret_cast<char*>(&submesh_count), sizeof(uint32_t));
        if (!vf || submesh_count == 0) {
            SDL_Log("LoadModels: failed to read submesh count from: %s", pm.vert_path.c_str());
            assert(false && "LoadModels: bad submesh count");
            continue;
        }
        std::vector<SubMeshFileEntry> entries(submesh_count);
        vf.read(reinterpret_cast<char*>(entries.data()), submesh_count * sizeof(SubMeshFileEntry));
        if (!vf) {
            SDL_Log("LoadModels: failed to read submesh entries from: %s", pm.vert_path.c_str());
            assert(false && "LoadModels: bad submesh entries");
            continue;
        }

        size_t header_size = sizeof(uint32_t) + submesh_count * sizeof(SubMeshFileEntry);
        vf.seekg(0, std::ios::end);
        size_t file_size = vf.tellg();
        size_t vdata_size = file_size - header_size;
        if (vdata_size == 0 || vdata_size % sizeof(PosUVNormal) != 0) {
            SDL_Log("LoadModels: invalid vertex data size in: %s", pm.vert_path.c_str());
            assert(false && "LoadModels: invalid vertex data size");
            continue;
        }
        uint32_t vcount = safe_u32(vdata_size / sizeof(PosUVNormal));

        // --- 2. размер индексного файла (без роста staging) ---
        std::ifstream indf(pm.ind_path, std::ios::binary);
        if (!indf) {
            SDL_Log("LoadModels: failed to open index file: %s", pm.ind_path.c_str());
            assert(false && "LoadModels: failed to open index file");
            continue;
        }
        indf.seekg(0, std::ios::end);
        size_t isize = indf.tellg();
        indf.seekg(0, std::ios::beg);
        if (isize == 0 || isize % sizeof(uint32_t) != 0) {
            SDL_Log("LoadModels: index file empty or invalid: %s", pm.ind_path.c_str());
            assert(false && "LoadModels: invalid index file");
            continue;
        }
        uint32_t icount = safe_u32(isize / sizeof(uint32_t));

        // Все проверки пройдены — теперь растим staging и читаем данные.
        // --- 3. вершины ---
        vf.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
        size_t vbase = staging_vertices.size();
        staging_vertices.resize(vbase + vcount, PosUVNormal{});
        vf.read(reinterpret_cast<char*>(staging_vertices.data() + vbase), vdata_size);
        if (!vf) {
            SDL_Log("LoadModels: incomplete vertex read (%zu / %zu) from %s",
                size_t(vf.gcount()), vdata_size, pm.vert_path.c_str());
            assert(false && "LoadModels: incomplete vertex read");
        }

        // --- 4. индексы ---
        size_t ibase = staging_indices.size();
        staging_indices.resize(ibase + icount, 0);
        indf.read(reinterpret_cast<char*>(staging_indices.data() + ibase), isize);
        if (!indf) {
            SDL_Log("LoadModels: incomplete index read (%zu / %zu) from %s",
                size_t(indf.gcount()), isize, pm.ind_path.c_str());
            assert(false && "LoadModels: incomplete index read");
        }

        // --- 5. сабмеши: смещения от курсоров + bounding sphere ---
        pm.model->submeshes.clear();
        pm.model->submeshes.reserve(entries.size());
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
                for (uint32_t i = 0; i < e.vertexCount; ++i) {
                    const PosUVNormal& v = staging_vertices[vbase + e.vertexOffset + i];
                    center += glm::vec3(v.x, v.y, v.z);
                }
                center /= static_cast<float>(e.vertexCount);

                float radius = 0.0f;
                for (uint32_t i = 0; i < e.vertexCount; ++i) {
                    const PosUVNormal& v = staging_vertices[vbase + e.vertexOffset + i];
                    float d = glm::distance(center, glm::vec3(v.x, v.y, v.z));
                    if (d > radius) radius = d;
                }
                sub.sphere = glm::vec4(center, radius);
            }
            pm.model->submeshes.push_back(sub);
        }

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
