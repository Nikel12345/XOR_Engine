#pragma once
#include <functional>
#include <SDL3/SDL.h>
#include "BufferData.h"

class BufferManager;
struct TransferBufferData;


struct UploadTask {
	BufferData* dst_buffer_data = nullptr;
	TransferBufferData* tbd = nullptr;
	Uint32 dst_offset = 0;
	Uint32 tb_offset = 0;
	Uint32 size = 0;
	Uint32 written_size = 0;
	Uint32 additional_offset = 0;
	bool resize_dst_buf_only = false;
};

// cp — для инструкции, которая заливает КОПИРОВАНИЕМ GPU→GPU (SDL_CopyGPUBufferToBuffer);
// та, что пишет через transfer-буфер, работает с task и cp не трогает.
using UpdateInstructionUpdaterFunc = std::function<void(SDL_GPUCopyPass* cp, BufferManager*, UploadTask&)>;
using UpdateInstructionSizeFunc = std::function <uint32_t()>;
using UpdateInstructionOffsetFunc = std::function <uint32_t()>;

struct UpdateInstruction {
	BufferData* buffer_data = nullptr;
	UpdateInstructionUpdaterFunc updater = nullptr;
	UpdateInstructionSizeFunc size_fn = nullptr;
	UpdateInstructionOffsetFunc offset_fn = nullptr;
};

struct ReadBackTask {
	BufferData* src_buffer_data = nullptr;
	TransferBufferData* tbd = nullptr;
	Uint32 src_offset = 0;
	Uint32 tb_offset = 0;
	Uint32 size = 0;
	Uint32 read_size = 0;
};

using ReadBackInstructionReaderFunc = std::function<void(BufferManager*, ReadBackTask&)>;
using ReadBackInstructionSizeFunc = std::function <uint32_t()>;

struct ReadBackInstruction {
	BufferData* buffer_data = nullptr;
	ReadBackInstructionReaderFunc reader = nullptr;
	ReadBackInstructionSizeFunc size_fn = nullptr;
};