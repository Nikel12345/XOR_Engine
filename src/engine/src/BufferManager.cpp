#include "PCH.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "LightStruct.h"

BufferManager::BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager) : dev(device), trm(transfer_manager) {
    using namespace DefaultBuffersNames;
	CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

	CreateBufferData(DEFAULT_TEX_STATE_RANK_BUFFER, sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_TEX_STATE_INDEX_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	CreateBufferData(DEFAULT_TEX_STATE_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);

    CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, BufferDataType::Dynamic)
        ->usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;

    CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 40, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    CreateBufferData(DEFAULT_OUT_PIB_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    CreateBufferData(DEFAULT_ENTITY_TO_CMD_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

    CreateBufferData(UI_TEXT_RANK_BUFFER,     sizeof(uint32_t) * 2 * 64,  BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    CreateBufferData(UI_TEXT_INDEX_BUFFER,    sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    CreateBufferData(UI_TEXT_BUFFER,          sizeof(uint32_t) * 4096,    BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

    CreateBufferData(UI_FONT_UVL_BUFFER, sizeof(uint32_t) * 4 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
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
    pending_bakes.push_back(ptr);

    return ptr;
}

void BufferManager::BakePending()
{
    if (pending_bakes.empty()) return;

    for (BufferData* data : pending_bakes) {
        if (!data) continue;
        if (data->usage == 0) continue;
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

    std::erase_if(pending_bakes, [](BufferData* d) {
        if (!d) return true;
        if (d->type == BufferDataType::Static) return d->Static.buffer != nullptr;
        for (int i = 0; i < BUFFERING_LEVEL; i++)
            if (!d->Dynamic.buffers[i]) return false;
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

