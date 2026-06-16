#pragma once
#include <functional>
#include <SDL3/SDL.h>
#include "BufferData.h"

class BufferManager;



struct UploadTask {
	BufferData* dst_buffer_data = nullptr;
	Uint32 dst_offset = 0;
	Uint32 tb_offset = 0;
	Uint32 size = 0;
	Uint32 written_size = 0;
	// Базовое смещение в буфере-назначении (append-точка). Известно уже на этапе
	// построения задач — поэтому EnsureBufferCapacity может отличить заливку с 0 от
	// дозаписи в конец и выделить буфер на (база + новый кусок), а не только на кусок.
	// Base offset in the destination buffer (append point). Known already at task-build
	// time, so EnsureBufferCapacity can tell a from-zero upload from an append and size
	// the buffer for (base + new chunk), not just the chunk.
	Uint32 additional_offset = 0;
	bool resize_dst_buf_only = false;
};

using UpdateInstructionUpdaterFunc = std::function<void(SDL_GPUCopyPass* cp, BufferManager*, UploadTask&)>;
using UpdateInstructionSizeFunc = std::function <uint32_t()>;
// Возвращает базовое смещение назначения (append-точку). Считывается до построения
// задач, чтобы корректно учесть его в требуемой ёмкости буфера.
// Returns the destination base offset (append point). Read before task building so it
// is accounted for in the required buffer capacity.
using UpdateInstructionOffsetFunc = std::function <uint32_t()>;

struct UpdateInstruction {
	BufferData* buffer_data = nullptr;
	UpdateInstructionUpdaterFunc updater = nullptr;
	UpdateInstructionSizeFunc size_fn = nullptr;
	UpdateInstructionOffsetFunc offset_fn = nullptr;
};

struct ReadBackTask {
	BufferData* src_buffer_data = nullptr;
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