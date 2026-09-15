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
	inline constexpr const char* DEFAULT_TRANSFORM_BUFFER = "DefaultTransformBuffer";
	inline constexpr const char* DEFAULT_CAMERA_BUFFER = "cameraBuffer";
	inline constexpr const char* DEFAULT_LIGHT_BUFFER = "lightBuffer";
	inline constexpr const char* DEFAULT_POSITION_INDEX_BUFFER = "DefaultPositionIndexBuffer";
	inline constexpr const char* DEFAULT_INSTANCE_BUFFER = "DefaultInstanceBuffer";
	inline constexpr const char* DEFAULT_LIGHT_CAMERA_BUFFER = "DefaultLightCameraBuffer";

	inline constexpr const char* DEFAULT_TEX_STATE_RANK_BUFFER   = "DefaultTexStateRankBuffer";
	inline constexpr const char* DEFAULT_TEX_STATE_INDEX_BUFFER  = "DefaultTexStateIndexBuffer";
	inline constexpr const char* DEFAULT_TEX_STATE_BUFFER        = "DefaultTexStateBuffer";

	inline constexpr const char* DEFAULT_INDIRECT_BUFFER = "DefaultIndirectBuffer";
	inline constexpr const char* DEFAULT_BOUND_SPHERE_BUFFER = "DefaultBoundSphereBuffer";
	inline constexpr const char* DEFAULT_OUT_PIB_BUFFER = "DefaultOutPibBuffer";
	inline constexpr const char* DEFAULT_ENTITY_TO_CMD_BUFFER = "DefaultEntityToCmdBuffer";

	inline constexpr const char* UI_TEXT_RANK_BUFFER     = "UITextRank";
	inline constexpr const char* UI_TEXT_INDEX_BUFFER    = "UITextIndex";
	inline constexpr const char* UI_TEXT_BUFFER          = "UITextBuffer";

	inline constexpr const char* UI_FONT_UVL_BUFFER      = "FontUVL";
};

struct PendingDestroy {
	SDL_GPUBuffer* buf;
	uint64_t ready_at = 0;
};

struct BufferDataNameHash {
	size_t operator()(BufferDataName n) const noexcept {
		return std::hash<std::string_view>{}(n ? std::string_view(n) : std::string_view{});
	}
};
struct BufferDataNameEq {
	bool operator()(BufferDataName a, BufferDataName b) const noexcept {
		if (a == b) return true;
		if (!a || !b) return false;
		return std::string_view(a) == std::string_view(b);
	}
};
using BufferDataRegistry =
	std::unordered_map<BufferDataName, std::unique_ptr<BufferData>, BufferDataNameHash, BufferDataNameEq>;

class BufferManager
{
public:
	BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager);
	BufferData* CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour = ResizeBehaviour::RESIZE_ONLY, ResourceTag tags = ResourceTag::None);

	void BakePending();

	void CreatePrePassUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);
	void CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);

	// updater не зовётся в двух случаях: size_fn вернула 0 (этим и выражают гейт по ревизии) и
	// у буфера ещё нет тела — BakePending не создал его, пока usage никто не объявил. Во втором
	// случае инструкция пропускается целиком и оживает сама, как только тело появится.
	void CreateUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn = nullptr);
	void CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn = nullptr);

	void CreateReadBackInstruction(BufferData& buffer_data, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn);
	void CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn);

	void CreatePostReadbackUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);
	void CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);

	TransferBufferData* ExecutePrePassUpdateInstruction(SDL_GPUCopyPass* cp);
	void ExecutePrePassUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	TransferBufferData* ExecuteUpdateInstructions(SDL_GPUCopyPass* cp);
	void ExecuteUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	TransferBufferData* ExecuteReadBackInstructionsSize();
	void ExecuteDownloadTasks(SDL_GPUCopyPass* cp, uint8_t idx);
	void ExecuteReadBackInstructionsReader();

	TransferBufferData* ExecutePostReadbackInstructions(SDL_GPUCopyPass* cp);
	void ExecutePostreadBackUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx);

	bool BindGPUIndexBuffer(SDL_GPURenderPass* rp, const BufferData* buffer_data, Uint32 offset);
	bool BindGPUVertexBuffers(SDL_GPURenderPass* rp, const std::vector<BufferData*>& buffers_data);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, std::initializer_list<const char*> names, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUComputeRO_Buffers(SDL_GPUComputePass* cmp, uint32_t slot, const std::vector<BufferData*>& buffers_data, uint8_t frame);

	void UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data);
	// ⚠️ ПО УМОЛЧАНИЮ НЕ ИСПОЛЬЗУЙ (в т.ч. нейросети): это перф-КОМПРОМИСС для ГОРЯЧЕГО пути с
	// большими объёмами (transform/instance/bound-sphere — тысячи-миллионы строк за кадр), где
	// лишний memcpy и промежуточный буфер реально стоят. Для обычной/редкой заливки бери
	// UploadToTransferBuffer — проще и безопаснее. Подводные камни прямого указателя: mapped-память
	// может быть write-combined (ТОЛЬКО писать, не читать — чтение медленное/некогерентное), и легко
	// ошибиться с размером/выравниванием (никто не проверит за тебя).
	void* AcquireTransferWritePtr(UploadTask* task, Uint32 size);
	std::span<const std::byte> ReadFromTransferBuffer(ReadBackTask* task, uint32_t size);

	void TrashBuffers(uint64_t fences_done);

	BufferData* GetBufferData(BufferDataName name);
	const BufferDataRegistry& GetBuffersData() const { return buffers_data; }
	~BufferManager();

	std::atomic<uint8_t> logic_index{ 0 };

	inline SDL_GPUBuffer* _GetGPUBufferForFrame(const BufferData* data, uint8_t any_frame) const
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
	std::vector<BufferData*> pending_bakes;
	BufferDataRegistry buffers_data;

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

