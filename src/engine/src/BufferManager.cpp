#include "PCH.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "LightStruct.h"
#include "PositionStructure.h"   // GeometryStreams::* — имена стрим-буферов пула

BufferManager::BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager) : dev(device), trm(transfer_manager) {
    using namespace DefaultBuffersNames;
	// Стримы пула PosUVNormPool (см. PositionStructure.h): вершины моделей по группам, каждый
	// стрим — свой буфер (растут независимо через RESIZE_AND_COPY, но В НОГУ по элементам —
	// канон продвижения у ModelManager). Ёмкости = общий запас вершин × страйд стрима, чтобы
	// стримы вмещали одинаковое число вершин (старый монолит: 8190600 Б / 44 Б ≈ 186k вершин).
	// USAGE-ФЛАГОВ ЗДЕСЬ НЕТ: BufferData::usage наполняют декларации до бейка (CreateVertexShader —
	// VERTEX/INDEX; Create*Program — storage-роли), см. BufferData.h. Здесь — только размеры и типы.
	constexpr Uint32 BASE_VERTEX_CAPACITY = 186150;
	CreateBufferData(GeometryStreams::VERTEX_POS_BUFFER,     BASE_VERTEX_CAPACITY * 12, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(GeometryStreams::VERTEX_UV_BUFFER,      BASE_VERTEX_CAPACITY * 8,  BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(GeometryStreams::VERTEX_NORMTAN_BUFFER, BASE_VERTEX_CAPACITY * 24, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	// Индексный буфер пула (у одного пула — один): общее индексное пространство всех его моделей,
	// финализируется индексным апдейтером вместе со стримами. Имя — в PositionStructure.h.
	CreateBufferData(PosUVNormPool::INDEX_BUFFER, 8190006, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, BufferDataType::Dynamic);
	// Per-instance данные (8 байт/строку). Dynamic — возможны удаления; авторесайз как у трансформов.
	CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, BufferDataType::Dynamic);

    // Индирект ПО-КАМЕРНО, правится компьютом (scatter пишет num_instances) — COMPUTE_STORAGE_WRITE
    // выведется из декларации scatter-программы. РУЧНОЙ тег INDIRECT: источник indirect-draw —
    // свойство раскладки батчей (BatchLayout::indirectBuffer, резолв после бейка),
    // Create*-декларации нет и не будет.
    CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, BufferDataType::Dynamic)
        ->usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;
    // GPU-каллинг: сферы по строкам трансформов + КОМПАКТНЫЙ out_pib + entity->cmd.
    // Все Dynamic (per-slot). out_pib пишет scatter (RW). entity->cmd — RO вход scatter.
    CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 40, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_OUT_PIB_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_ENTITY_TO_CMD_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic);
}

BufferData* BufferManager::CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour)
{
    auto it = buffers_data.find(name);
    if (it != buffers_data.end()) {
        SDL_Log("Buffer '%s' already exists, returning existing buffer data.", name);
        return it->second.get();
    }

    auto data = std::make_unique<BufferData>();
    data->type = type;
    data->resize_behaviour = resize_behaviour;
	data->debug_name = name;
    // usage здесь НЕ трогаем (остаётся 0): его наполнят декларации до бейка — см. BufferData.h.

    // ОТЛОЖЕННОЕ СОЗДАНИЕ: здесь только РАЗМЕРЫ, сам SDL_GPUBuffer создаст BakePending (начало
    // PrepareFunc). Потребителям это безразлично: они держат BufferData* и берут GPU-хэндл через
    // _GetGPUBufferForFrame на бинде — до бейка его просто никто не спрашивает.
    switch (type) {
        case BufferDataType::Static:
			data->Static.buffer_size = size;
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++) {
                data->Dynamic.buffer_size[i] = size;
            };
            break;
        }


    BufferData* ptr = data.get();
    buffers_data[name] = std::move(data);
    pending_bakes.push_back(ptr);   // GPU-буфер создаст ближайший BakePending

    return ptr;
}

// Дренаж отложенных созданий. Каждый кадр (начало PrepareFunc) — поэтому ресурс, заведённый
// игрой в любом кадре, живёт на GPU уже в этом же кадре. См. BufferManager.h.
void BufferManager::BakePending()
{
    if (pending_bakes.empty()) return;

    for (BufferData* data : pending_bakes) {
        if (!data) continue;
        // Ни одна декларация буфер не назвала — создавать не с чем (usage=0 SDL не примет).
        // Контракт: все декларации — до первого бейка после CreateBufferData.
        if (data->usage == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "BufferManager::BakePending: buffer '%s' has NO declared usage "
                "(no shader/program/tag referenced it) — GPU buffer NOT created.",
                data->debug_name.c_str());
            continue;
        }
        switch (data->type) {
        case BufferDataType::Static:
            if (!data->Static.buffer) {
                data->Static.buffer = CreateBuffer(data->Static.buffer_size, data->usage);
                if (!data->Static.buffer)
                    SDL_Log("BufferManager::BakePending: buffer '%s' failed.", data->debug_name.c_str());
            }
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++) {
                if (data->Dynamic.buffers[i]) continue;
                data->Dynamic.buffers[i] = CreateBuffer(data->Dynamic.buffer_size[i], data->usage);
                if (!data->Dynamic.buffers[i])
                    SDL_Log("BufferManager::BakePending: buffer '%s' [%d] failed.", data->debug_name.c_str(), i);
            }
            break;
        }
    }

    pending_bakes.clear();
}

void BufferManager::TrashBuffers(uint64_t fences_done)
{
    // Дренаж на sim. Стамп при первом визите (позже момента постановки — консервативно, безопасно);
    // release после BUFFERING_LEVEL завершённых render-fence: fence одного queue сигналят в порядке
    // сабмита, значит все кадры, отправленные до стампа, к этому моменту дошли.
    auto it = trash.begin();
    while (it != trash.end()) {
        if (it->ready_at == 0) { it->ready_at = fences_done + BUFFERING_LEVEL; ++it; }
        else if (fences_done >= it->ready_at) {
            SDL_ReleaseGPUBuffer(dev, it->buf);
            it = trash.erase(it);
        }
        else ++it;
    }
}

BufferManager::~BufferManager()
{
    for (auto& [name, data] : buffers_data)
    {
        if (!data) continue;

        switch (data->type)
        {
        case BufferDataType::Static:
            if (data->Static.buffer)
                SDL_ReleaseGPUBuffer(dev, data->Static.buffer);
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++)
                if (data->Dynamic.buffers[i])
                    SDL_ReleaseGPUBuffer(dev, data->Dynamic.buffers[i]);
            break;
        }
    }

    buffers_data.clear();
}

SDL_GPUBuffer* BufferManager::CreateBuffer(Uint32 size, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo info{};
    info.size = size;
    info.usage = usage;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(dev, &info);
    if (!buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Buffer creation failed: %s", SDL_GetError());
        return nullptr;
    }
    return buffer;
}

