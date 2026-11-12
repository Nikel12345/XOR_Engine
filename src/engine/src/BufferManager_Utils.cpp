#include "PCH.h"
#include "BufferManager.h"

void BufferManager::EnsureBufferCapacity(SDL_GPUCopyPass* cp, BufferData* data, Uint32 req_buffer_size, uint8_t li)
{
    if (!data) {
        SDL_Log("EnsureBufferCapacity: data is null");
        return;
    }

    const uint8_t body = data->type == BufferDataType::Static ? 0 : li;
    if (data->buffer_size[body] >= req_buffer_size) {
        return;
    }

    Uint32 old_size = data->buffer_size[body];
    SDL_GPUBuffer* old_buffer = data->buffers[body];

    SDL_Log("BM: Resizing %s buffer '%s' [%u] from %u to %u",
        data->type == BufferDataType::Static ? "STATIC" : "DYNAMIC",
        data->debug_name.c_str(), (Uint32)body, old_size, req_buffer_size);

    SDL_GPUBuffer* new_buffer = CreateBuffer(req_buffer_size, data->usage);

    if (data->resize_behaviour == ResizeBehaviour::RESIZE_AND_COPY) {
        Uint32 copy_size = data->used_buffer_size[body];
        if (copy_size > old_size) copy_size = old_size;
        if (cp && old_buffer && copy_size > 0) {
            SDL_GPUBufferLocation src{ old_buffer, 0 };
            SDL_GPUBufferLocation dst{ new_buffer, 0 };
            SDL_CopyGPUBufferToBuffer(cp, &src, &dst, copy_size, false);
        }
    }

    data->buffers[body] = new_buffer;
    data->buffer_size[body] = req_buffer_size;

    trash.push_back({ old_buffer });
}

BufferData* BufferManager::GetBufferData(BufferDataName name)
{
    auto it = buffers_data.find(name);
    if (it != buffers_data.end()) {
        return it->second.get();
    }

    SDL_Log("BM operator::Buffer '%s' not found", name);
    return nullptr;
}
