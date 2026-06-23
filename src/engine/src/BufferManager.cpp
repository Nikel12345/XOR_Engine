#include "PCH.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "LightStruct.h"

BufferManager::BufferManager(SDL_GPUDevice* device) : ResourceManager(device) {
    using namespace DefaultBuffersNames;
	CreateBufferData(DEFAULT_VERTEX_BUFFER, 8190600, SDL_GPU_BUFFERUSAGE_VERTEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(DEFAULT_INDEX_BUFFER, 8190006, SDL_GPU_BUFFERUSAGE_INDEX, BufferDataType::Static, ResizeBehaviour::RESIZE_AND_COPY);
	CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Static);
	CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	// Per-instance данные (8 байт/строку). Dynamic — возможны удаления; авторесайз как у трансформов.
	CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);
	
    CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_ENTITY_TO_BATCH_BUFFER, BASE_TB_SIZE / 16 / 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Static);
    CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 4/ 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, BufferDataType::Static);
    CreateBufferData(DEFAULT_COUNT_BUFFER, 10, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_OFFSET_BUFFER, 1, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_OUT_TRANSFORM_BUFFER, 1, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, BufferDataType::Static);
    CreateBufferData(DEFAULT_OUT_INDIRECT_BUFFER, 1, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, BufferDataType::Static);
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
    switch (type) {
        case BufferDataType::Static:
            data->Static.buffer = CreateBuffer(size, usage);
			data->Static.buffer_size = size;
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++) {
                data->Dynamic.buffers[i] = CreateBuffer(size, usage);
                data->Dynamic.buffer_size[i] = size;
            };
            break;
        }


    BufferData* ptr = data.get();
    buffers_data[name] = std::move(data);

    return ptr;
}

void BufferManager::TrashBuffers()
{
    auto it = trash.begin();
    while (it != trash.end()) {
        if (it->frame_ready <= 0) {
            SDL_ReleaseGPUBuffer(dev, it->buf);
            it = trash.erase(it);
        }
        else {
            it->frame_ready--;
            ++it;
        }
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

