#pragma once
#include "config.h"
#include "SDL3/SDL_gpu.h"
#include "ResourceTags.h"

enum struct BufferDataType {
	Static,
	Dynamic
};

enum struct ResizeBehaviour {
    RESIZE_ONLY,
    RESIZE_AND_COPY
};

struct BufferData {
    BufferDataType type = BufferDataType::Static;
    ResizeBehaviour resize_behaviour = ResizeBehaviour::RESIZE_ONLY;
    std::string debug_name;
    ResourceTag tags = ResourceTag::None;

    // Тело Static лежит в [0], остальные ячейки у него пустые: индекс слота напрямую даёт nullptr.
    SDL_GPUBuffer* buffers[BUFFERING_LEVEL]{};
    Uint32 buffer_size[BUFFERING_LEVEL]{};
    Uint32 used_buffer_size[BUFFERING_LEVEL]{};

    SDL_GPUBufferUsageFlags usage = 0;
};
