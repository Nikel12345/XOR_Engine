#include "PCH.h"
#include "BufferManager.h"

void BufferManager::EnsureBufferCapacity(SDL_GPUCopyPass* cp, BufferData* data, Uint32 req_buffer_size, uint8_t li)
{
    if (!data) {
        SDL_Log("EnsureBufferCapacity: data is null");
        return;
    }

    const auto usage = data->usage;

    switch (data->type)
    {
    case BufferDataType::Static:
    {
        if (data->Static.buffer_size >= req_buffer_size) {
            return;
        }

        Uint32 old_size = data->Static.buffer_size;
        SDL_GPUBuffer* old_buffer = data->Static.buffer;

        SDL_Log("BM: Resizing STATIC buffer '%s' from %u to %u",
            data->debug_name.c_str(), old_size, req_buffer_size);

        SDL_GPUBuffer* new_buffer = CreateBuffer(req_buffer_size, usage);

        // RESIZE_AND_COPY: переносим уже залитые данные в новый буфер до утилизации старого.
        if (data->resize_behaviour == ResizeBehaviour::RESIZE_AND_COPY) {
            Uint32 copy_size = data->Static.used_buffer_size;
            if (copy_size > old_size) copy_size = old_size;
            if (cp && old_buffer && copy_size > 0) {
                SDL_GPUBufferLocation src{ old_buffer, 0 };
                SDL_GPUBufferLocation dst{ new_buffer, 0 };
                SDL_CopyGPUBufferToBuffer(cp, &src, &dst, copy_size, false);
            }
        }

        data->Static.buffer = new_buffer;
        data->Static.buffer_size = req_buffer_size;

        trash.push_back({ old_buffer, BUFFERING_LEVEL });
        return;
    }

    case BufferDataType::Dynamic:
    {
        if (data->Dynamic.buffer_size[li] >= req_buffer_size) {
            return;
        }
        Uint32 old_size = data->Dynamic.buffer_size[li];
        SDL_GPUBuffer* old_buffer = data->Dynamic.buffers[li];

        SDL_Log("BM: Resizing DYNAMIC buffer '%s' [li=%u] from %u to %u",
            data->debug_name.c_str(), (Uint32)li, old_size, req_buffer_size);

        SDL_GPUBuffer* new_buffer = CreateBuffer(req_buffer_size, usage);

        if (data->resize_behaviour == ResizeBehaviour::RESIZE_AND_COPY) {
            Uint32 copy_size = data->Dynamic.used_buffer_size[li];
            if (copy_size > old_size) copy_size = old_size;
            if (cp && old_buffer && copy_size > 0) {
                SDL_GPUBufferLocation src{ old_buffer, 0 };
                SDL_GPUBufferLocation dst{ new_buffer, 0 };
                SDL_CopyGPUBufferToBuffer(cp, &src, &dst, copy_size, false);
            }
        }

        data->Dynamic.buffers[li] = new_buffer;
        data->Dynamic.buffer_size[li] = req_buffer_size;

        trash.push_back({ old_buffer, BUFFERING_LEVEL });
        return;
    }

    default:
        SDL_Log("EnsureBufferCapacity: unknown buffer type");
        return;
    }
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

//BufferData* BufferManager::operator[](BufferDataName name)
//{
//    auto it = buffers_data.find(name);
//    if (it != buffers_data.end()) {
//        return it->second.get();
//    }
//
//    SDL_Log("BM operator::Buffer '%s' not found", name);
//    return nullptr;
//}
