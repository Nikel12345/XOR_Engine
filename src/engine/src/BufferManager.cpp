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
	constexpr Uint32 BASE_VERTEX_CAPACITY = 186150;
	CreateBufferData(GeometryStreams::VERTEX_POS_BUFFER,     BASE_VERTEX_CAPACITY * 12, SDL_GPU_BUFFERUSAGE_VERTEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(GeometryStreams::VERTEX_UV_BUFFER,      BASE_VERTEX_CAPACITY * 8,  SDL_GPU_BUFFERUSAGE_VERTEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(GeometryStreams::VERTEX_NORMTAN_BUFFER, BASE_VERTEX_CAPACITY * 24, SDL_GPU_BUFFERUSAGE_VERTEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(DEFAULT_INDEX_BUFFER, 8190006, SDL_GPU_BUFFERUSAGE_INDEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	// PIB/трансформы/камеры читает и графика, и culling_pib.comp → оба usage-флага.
	CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
	// Сырой PIB читает ТОЛЬКО каллинг (culling_pib.comp) — графика с переходом на компактацию
	// перешла на out_pib (вершинники ссылаются на DefaultOutPibBuffer). GRAPHICS_STORAGE_READ был
	// протухшим наследством и снят: авто-сбор флагов его ни из одной декларации не выводит.
	CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
	// Per-instance данные (8 байт/строку). Dynamic — возможны удаления; авторесайз как у трансформов.
	CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
	
    // Индирект теперь ПО-КАМЕРНО и правится компьютом (scatter атомарно пишет num_instances),
    // поэтому + COMPUTE_STORAGE_WRITE. Заливается per-frame с num_instances=0, scatter накапливает.
    CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, BufferDataType::Dynamic);
    // GPU-каллинг: сферы по строкам трансформов + КОМПАКТНЫЙ out_pib + entity->cmd.
    // Все Dynamic (per-slot). out_pib пишет scatter (RW). entity->cmd — RO вход scatter.
    CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 40, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_OUT_PIB_BUFFER, BASE_TB_SIZE / 16 / 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_ENTITY_TO_CMD_BUFFER, BASE_TB_SIZE / 16 / 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
}

BufferData* BufferManager::CreateBufferData(BufferDataName name, Uint32 size, SDL_GPUBufferUsageFlags usage, BufferDataType type, ResizeBehaviour resize_behaviour)
{
    auto it = buffers_data.find(name);
    if (it != buffers_data.end()) {
        SDL_Log("Buffer '%s' already exists, returning existing buffer data.", name);
        return it->second.get();
    }

    auto data = std::make_unique<BufferData>();
    data->type = type;
    data->resize_behaviour = resize_behaviour;
    data->usage = usage;
	data->debug_name = name;

    // ── НАМЕРЕНИЕ (declared at creation), а не выведенное использование ──
    // INDEX: индексный буфер биндится напрямую по имени (BindGPUIndexBuffer), деклараций нет.
    // INDIRECT: источник indirect-draw зашит в RenderPassStandardBody — декларации пока нет.
    // VERTEX сюда БОЛЬШЕ НЕ ВХОДИТ: вершинные буфера объявляются перечислением в
    // CreateVertexShader (стримы пула) — флаг выводится там, как storage-буфера у sp.
    constexpr SDL_GPUBufferUsageFlags kIntent = SDL_GPU_BUFFERUSAGE_INDEX
                                              | SDL_GPU_BUFFERUSAGE_INDIRECT;
    data->debug_usage |= (usage & kIntent);

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

// Имя одного бита usage-флага буфера.
static const char* BufferUsageFlagName(SDL_GPUBufferUsageFlags bit)
{
    switch (bit) {
    case SDL_GPU_BUFFERUSAGE_VERTEX:                 return "VERTEX";
    case SDL_GPU_BUFFERUSAGE_INDEX:                  return "INDEX";
    case SDL_GPU_BUFFERUSAGE_INDIRECT:               return "INDIRECT";
    case SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ:  return "GRAPHICS_STORAGE_READ";
    case SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ:   return "COMPUTE_STORAGE_READ";
    case SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE:  return "COMPUTE_STORAGE_WRITE";
    default:                                         return "UNKNOWN";
    }
}

// Разложить маску в строку "A|B|C" (пусто → "-").
static std::string BufferUsageFlagsToString(SDL_GPUBufferUsageFlags flags)
{
    std::string out;
    for (uint32_t bit = 1; bit; bit <<= 1) {
        if (!(flags & bit)) continue;
        if (!out.empty()) out += "|";
        out += BufferUsageFlagName(bit);
    }
    return out.empty() ? "-" : out;
}

// ASCII-only: SDL_Log в Windows-консоли калечит кириллицу.
void BufferManager::ReportUsageMismatch()
{
    SDL_Log("=== BUFFER usage flags: manual (used) vs auto-collected (debug_usage) ===");
    size_t mismatches = 0;
    for (const auto& [name, data] : buffers_data) {
        if (!data) continue;
        const SDL_GPUBufferUsageFlags missed = data->usage & ~data->debug_usage;   // задано вручную, НЕ выведено
        const SDL_GPUBufferUsageFlags extra  = data->debug_usage & ~data->usage;   // выведено, вручную НЕТ
        if (!missed && !extra) continue;
        ++mismatches;
        SDL_Log("  %-30s mismatch: manual=[%s] auto=[%s] | NOT_DERIVED=[%s] | EXTRA=[%s]",
            data->debug_name.c_str(),
            BufferUsageFlagsToString(data->usage).c_str(),
            BufferUsageFlagsToString(data->debug_usage).c_str(),
            BufferUsageFlagsToString(missed).c_str(),
            BufferUsageFlagsToString(extra).c_str());
    }
    if (mismatches == 0) SDL_Log("  no mismatches: intent (INDEX/INDIRECT) + derived (VERTEX + storage) == manual.");
    else SDL_Log("  %zu mismatch(es) -- each one is a real over-grant or a real gap.", mismatches);
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

