#include "PCH.h"
#include "BufferManager.h"

BufferManager::BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager) : dev(device), trm(transfer_manager) {
}

BufferData* BufferManager::CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour, ResourceTag tags)
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
	data->tags = tags;
    const int bodies = type == BufferDataType::Static ? 1 : BUFFERING_LEVEL;
    for (int i = 0; i < bodies; i++)
        data->buffer_size[i] = size;

    BufferData* ptr = data.get();
    buffers_data[name] = std::move(data);
    pending_bakes.push_back(ptr);

    return ptr;
}

void BufferManager::BakePending()
{
    if (pending_bakes.empty()) return;

    for (BufferData* data : pending_bakes) {
        if (!data) continue;
        if (data->usage == 0) continue;
        const int bodies = data->type == BufferDataType::Static ? 1 : BUFFERING_LEVEL;
        for (int i = 0; i < bodies; i++) {
            if (data->buffers[i]) continue;
            data->buffers[i] = CreateBuffer(data->buffer_size[i], data->usage);
            if (!data->buffers[i])
                SDL_Log("BufferManager::BakePending: buffer '%s' [%d] failed.", data->debug_name.c_str(), i);
        }
    }

    std::erase_if(pending_bakes, [](BufferData* d) {
        if (!d) return true;
        const int bodies = d->type == BufferDataType::Static ? 1 : BUFFERING_LEVEL;
        for (int i = 0; i < bodies; i++)
            if (!d->buffers[i]) return false;
        return true;
    });
}

void BufferManager::TrashBuffers(uint64_t fences_done)
{
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
        for (SDL_GPUBuffer* buffer : data->buffers)
            if (buffer)
                SDL_ReleaseGPUBuffer(dev, buffer);
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

