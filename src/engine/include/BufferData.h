#pragma once
#include "config.h"
#include "SDL3/SDL_gpu.h"

enum struct BufferDataType {
	Static,
	Dynamic
};

// Как ведёт себя буфер при росте в EnsureBufferCapacity:
//   RESIZE_ONLY     — просто создать новый буфер большего размера (старое содержимое
//                     теряется). Подходит для буферов, которые всё равно переписываются
//                     целиком каждый кадр (dynamic) или пересобираются с нуля.
//   RESIZE_AND_COPY — перед заменой скопировать старые данные в новый буфер (GPU→GPU).
//                     Нужно append-буферам (вершины/индексы), куда дозаписываются модели.
// How the buffer behaves when it grows in EnsureBufferCapacity:
//   RESIZE_ONLY     — just create a bigger buffer (old contents are lost).
//   RESIZE_AND_COPY — copy old contents into the new buffer (GPU→GPU) before swapping.
enum struct ResizeBehaviour {
    RESIZE_ONLY,
    RESIZE_AND_COPY
};

struct BufferData {
    BufferDataType type = BufferDataType::Static;
    ResizeBehaviour resize_behaviour = ResizeBehaviour::RESIZE_ONLY;
    std::string debug_name;

    struct StaticBufferInfo {
        SDL_GPUBuffer* buffer = nullptr;
        Uint32 buffer_size = 0;
        Uint32 used_buffer_size = 0;
    } Static;

    struct DynamicBufferInfo {
        SDL_GPUBuffer* buffers[BUFFERING_LEVEL]{};
        Uint32 buffer_size[BUFFERING_LEVEL]{};
        Uint32 used_buffer_size[BUFFERING_LEVEL]{};
    } Dynamic;

    SDL_GPUBufferUsageFlags usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
};
