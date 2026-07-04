#pragma once
#pragma warning(disable: 26495)
#include <functional>
#include <deque>
#include <string_view>
#include <string>
#include <span>
#include <unordered_map>
#include <memory>
#include <atomic>
#include "Aliases.h"
#include "TransferManager.h"
#include "BufferData.h"
#include "BufferUpdateStruct.h"

namespace DefaultBuffersNames {
	inline constexpr const char* DEFAULT_VERTEX_BUFFER = "_DefaultVertexBuffer";
	inline constexpr const char* DEFAULT_INDEX_BUFFER = "_DefaultIndexBuffer";
	inline constexpr const char* DEFAULT_TRANSFORM_BUFFER = "_DefaultTransformBuffer";
	inline constexpr const char* DEFAULT_CAMERA_BUFFER = "_cameraBuffer";
	inline constexpr const char* DEFAULT_LIGHT_BUFFER = "_lightBuffer";
	inline constexpr const char* DEFAULT_POSITION_INDEX_BUFFER = "_DefaultPositionIndexBuffer";
	inline constexpr const char* DEFAULT_INSTANCE_BUFFER = "_DefaultInstanceBuffer";
	inline constexpr const char* DEFAULT_LIGHT_CAMERA_BUFFER = "DefaultLightCameraBuffer";

	inline constexpr const char* DEFAULT_INDIRECT_BUFFER = "DefaultIndirectBuffer";
	inline constexpr const char* DEFAULT_ENTITY_TO_BATCH_BUFFER = "DefaultEntityToBatchBuffer";
	inline constexpr const char* DEFAULT_BOUND_SPHERE_BUFFER = "DefaultBoundSphereBuffer";
	inline constexpr const char* DEFAULT_COUNT_BUFFER = "DefaultCountBuffer";
	inline constexpr const char* DEFAULT_OFFSET_BUFFER = "DefaultOffsetBuffer";
	inline constexpr const char* DEFAULT_OUT_TRANSFORM_BUFFER = "DefaultOutTransformBuffer";
	inline constexpr const char* DEFAULT_OUT_INDIRECT_BUFFER = "DefaultOutIndirectBuffer";
};

struct PendingDestroy {
	SDL_GPUBuffer* buf;
	uint64_t frame_ready;
};

class BufferManager
{
public:
	BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager);
	BufferData* CreateBufferData(BufferDataName name, Uint32 size, SDL_GPUBufferUsageFlags usage, BufferDataType type, ResizeBehaviour resize_behaviour = ResizeBehaviour::RESIZE_ONLY);

	void CreatePrePassUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);
	void CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);

	void CreateUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn = nullptr);
	void CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn = nullptr);

	void CreateReadBackInstruction(BufferData& buffer_data, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn);
	void CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn);

	void CreatePostReadbackUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);
	void CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);

	// Execute*, строящие таски, арендуют transfer-буфер у TransferManager и возвращают его.
	// Владелец fence фазы (prep-функция) обязан вернуть его через TransferManager::ReleaseTB
	// ПОСЛЕ ожидания fence. nullptr (пустая фаза) — допустим, ReleaseTB(nullptr) — no-op.
	TransferBufferData* ExecutePrePassUpdateInstruction(SDL_GPUCopyPass* cp);
	// Требует завершения работы GPU
	// Requre GPU idle
	void ExecutePrePassUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	TransferBufferData* ExecuteUpdateInstructions(SDL_GPUCopyPass* cp);
	// Требует завершения работы GPU
	// Requre GPU idle
	void ExecuteUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	TransferBufferData* ExecuteReadBackInstructionsSize();
	// Требует завершения работы GPU
	// Requre GPU idle
	void ExecuteDownloadTasks(SDL_GPUCopyPass* cp, uint8_t idx);
	void ExecuteReadBackInstructionsReader();

	TransferBufferData* ExecutePostReadbackInstructions(SDL_GPUCopyPass* cp);
	void ExecutePostreadBackUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	void BindGPUIndexBuffer(SDL_GPURenderPass* rp, Uint32 offset);
	void BindGPUVertexBuffer(SDL_GPURenderPass* rp, Uint32 offset, Uint32 slot);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, std::initializer_list<const char*> names, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUComputeRO_Buffers(SDL_GPUComputePass* cmp, uint32_t slot, const std::vector<BufferData*>& buffers_data, uint8_t frame);

	// Пишет в transfer-буфер самого таска (task->tbd) — одна функция на все фазы.
	void UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data);
	std::span<const std::byte> ReadFromTransferBuffer(ReadBackTask* task, uint32_t size);

	void TrashBuffers();

	BufferData* GetBufferData(BufferDataName name);
	~BufferManager();

	std::atomic<uint8_t> logic_index{ 0 };

	inline SDL_GPUBuffer* _GetGPUBufferForFrame(const BufferData* data, uint8_t any_frame) const // Можно использовать в logic loop и render loop
	{
		if (!data) return nullptr;

		switch (data->type)
		{
		case BufferDataType::Static:
			return data->Static.buffer;

		case BufferDataType::Dynamic:
			return data->Dynamic.buffers[any_frame];
		}

		return nullptr;
	};

private:
	void _CreateUpdateInstruction(BufferData* buffer_data, std::vector<UpdateInstruction>& target_vector, UpdateInstructionUpdaterFunc fn = nullptr, UpdateInstructionSizeFunc size_fn = nullptr, UpdateInstructionOffsetFunc offset_fn = nullptr);
	TransferBufferData* _ExecuteUpdateInstructions(SDL_GPUCopyPass* cp, std::vector<UpdateInstruction>& target_instr_vector, std::vector<UploadTask>& target_task_vector);
	TransferBufferData* _BuildUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector);
	TransferBufferData* _BuildDownloadTasks();
	void _ExecuteUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector, uint8_t idx);

	SDL_GPUBuffer* CreateBuffer(Uint32 size, SDL_GPUBufferUsageFlags usage);

	SDL_GPUDevice* dev = nullptr;
	TransferManager* trm = nullptr;
	std::unordered_map<BufferDataName, std::unique_ptr<BufferData>> buffers_data;

	std::vector<UploadTask> prepass_upload_tasks;
	std::vector<UploadTask> post_readback_upload_tasks;
	std::vector<UploadTask> upload_tasks;
	std::vector<ReadBackTask> download_tasks;

	std::vector<UpdateInstruction> prepass_update_instructions;
	std::vector<UpdateInstruction> post_readback_update_instructions;
	std::vector<UpdateInstruction> update_instructions;
	std::vector<ReadBackInstruction> readback_instructions;

	std::deque<PendingDestroy> trash;

	// Потокобезопасность — EnsureBufferCapacity:
	//
	// ЗАПРЕЩЕНО: один BufferData* в Undepended UI + любом Depended UI
	//   → Undepended (update_instructions)  || Depended (prepass_update_instructions)
	//   → Undepended (update_instructions)  || Depended (post_readback_instructions)
	//
	// РАЗРЕШЕНО: один BufferData* внутри Depended UI
	//   → prepass_update_instructions → ... → post_readback_instructions
	//   Гарантировано последовательным выполнением PrepareFuncPrepassDepended
	// PROHIBITED: One BufferData* in an Undependent UI + any Depended UI
	
	// THREAD SAFETY — EnsureBufferCapacity:
	// PROHIBITED: One BufferData* in an Undependent UI + any Depended UI
	// → Undependent (update_instructions) || Depended (prepass_update_instructions)
	// → Undependent (update_instructions) || Depended (post_readback_instructions)
	//
	// ALLOWED: One BufferData* within a Depended UI
	// → prepass_update_instructions → ... → post_readback_instructions
	// Guaranteed by sequential execution of PrepareFuncPrepassDepended
	void EnsureBufferCapacity(SDL_GPUCopyPass* cp, BufferData* buffer_data, Uint32 size, uint8_t idx);
};

