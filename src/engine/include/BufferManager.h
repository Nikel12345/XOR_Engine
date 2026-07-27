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
	// Вершинного буфера-монолита больше нет: геометрия моделей лежит ПО СТРИМАМ пула
	// PosUVNormPool (Pos/UV/NormTan) + его ИНДЕКСНЫЙ буфер — имена в PositionStructure.h
	// (GeometryStreams::*, PosUVNormPool::INDEX_BUFFER). У одного пула — один индексный буфер.
	inline constexpr const char* DEFAULT_TRANSFORM_BUFFER = "_DefaultTransformBuffer";
	inline constexpr const char* DEFAULT_CAMERA_BUFFER = "_cameraBuffer";
	inline constexpr const char* DEFAULT_LIGHT_BUFFER = "_lightBuffer";
	inline constexpr const char* DEFAULT_POSITION_INDEX_BUFFER = "_DefaultPositionIndexBuffer";
	inline constexpr const char* DEFAULT_INSTANCE_BUFFER = "_DefaultInstanceBuffer";
	inline constexpr const char* DEFAULT_LIGHT_CAMERA_BUFFER = "DefaultLightCameraBuffer";

	// Индиректы: теперь ПО-КАМЕРНО — num_cameras копий команд, num_instances у каждой
	// копии заполняет scatter-каллинг атомиком (компактный счётчик выживших). Блок 0 —
	// камера игрока, 1..L — световые. Draw читает свой блок через additional_offset.
	inline constexpr const char* DEFAULT_INDIRECT_BUFFER = "DefaultIndirectBuffer";
	inline constexpr const char* DEFAULT_BOUND_SPHERE_BUFFER = "DefaultBoundSphereBuffer";
	// Выход scatter-каллинга: КОМПАКТНЫЙ pib. num_cameras блоков по N int; внутри каждого
	// блока выжившие каждой команды упакованы в начало её диапазона [firstInstance, …).
	inline constexpr const char* DEFAULT_OUT_PIB_BUFFER = "DefaultOutPibBuffer";
	// entity -> глобальный индекс команды (model_batch) для scatter. Индекс сам кодирует
	// проход (команды сгруппированы по проходам; shadow первый → [0,S)). Гейт по ревизии.
	inline constexpr const char* DEFAULT_ENTITY_TO_CMD_BUFFER = "DefaultEntityToCmdBuffer";

	// ── UI-текст (концепт). Разреженный канал: текст есть лишь у малой доли энтити, поэтому
	// не держим полноразмерный массив на КАЖДЫЙ row трансформа, а раскладываем на 4 буфера
	// (см. UI_DataModule). Вершинник по row из OutPib проверяет бит присутствия, при попадании
	// считает rank (по WordBase) и читает INDEX[rank] → срез TEXT-буфера.
	inline constexpr const char* UI_TEXT_BITS_BUFFER     = "UITextBits";       // 1 бит присутствия на row (по 32 в uint)
	inline constexpr const char* UI_TEXT_WORDBASE_BUFFER = "UITextWordBase";   // пословный префикс-popcount (rank за O(1))
	inline constexpr const char* UI_TEXT_INDEX_BUFFER    = "UITextIndex";      // {offset,count} на текст-элемент (компактно, индекс = rank)
	inline constexpr const char* UI_TEXT_BUFFER          = "UITextBuffer";     // коды глифов всех строк подряд

	// GlyphUVL шрифта: code(glyph_index) → UVL глифа в глиф-атласе. Заполняет FontManager
	// (StoreGlyphUVL) из handle->texture_data после PackAtlases. Один общий буфер (реестр keyed
	// по стабильному const char*, per-font имя не завести — мультишрифт позже).
	inline constexpr const char* UI_FONT_UVL_BUFFER      = "__FontUVL";
};

struct PendingDestroy {
	SDL_GPUBuffer* buf;
	// Стамп освобождения: 0 = не проставлен; первый дренаж ставит fences_done + BUFFERING_LEVEL,
	// release при fences_done >= ready_at (дренаж — sim, счётчик render-fence тикает FenceThread).
	uint64_t ready_at = 0;
};

class BufferManager
{
public:
	BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager);
	// Usage-флагов в сигнатуре НЕТ: BufferData::usage наполняют ДЕКЛАРАЦИИ (CreateVertexShader,
	// Create*Program, ручной тег INDIRECT) до бейка — GPU-буфер создаётся по ним (см. BufferData.h).
	BufferData* CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour = ResizeBehaviour::RESIZE_ONLY);

	// Отложенная инициализация GPU-ресурсов. CreateBufferData только РЕГИСТРИРУЕТ обёртку (размеры)
	// и кладёт её в pending_bakes; сам SDL_GPUBuffer создаёт этот дренаж. Зовётся КАЖДЫЙ
	// кадр в начале Engine::PrepareFunc — до PackAtlases и сборки батчей, которые уже требуют
	// готовые GPU-хэндлы. Игровой апдейт идёт раньше prepare на том же sim-потоке
	// (ThreadController), поэтому ресурс, созданный в кадре N, создаётся на GPU в том же кадре N —
	// и все объявления (sp/материалы), сделанные до этого момента, успевают дать ему свои флаги.
	void BakePending();

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

	// Индексный буфер пула — из слепка батча (резолв принадлежностью пулу на сборке батча).
	// false = бинда НЕ было — вызывающий обязан пропустить draw (indexed indirect без индексов).
	bool BindGPUIndexBuffer(SDL_GPURenderPass* rp, const BufferData* buffer_data, Uint32 offset);
	// Вершинные стримы батча ОДНИМ вызовом со слота 0; порядок списка = порядок слотов пайплайна.
	// false = резолв сорвался, бинд НЕ сделан — вызывающий обязан пропустить draw (иначе UB:
	// пайплайн ждёт в слоте k страйд стрима k, а там мусор/чужой буфер).
	bool BindGPUVertexBuffers(SDL_GPURenderPass* rp, const std::vector<BufferData*>& buffers_data);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 offset, std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	void BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, std::initializer_list<const char*> names, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(const std::vector<BufferData*>& buffers_data, uint8_t render_frame);
	std::vector<SDL_GPUStorageBufferReadWriteBinding> BuildBindGPUComputeRWBuffers(std::initializer_list<const char*> names, uint8_t render_frame);

	void BindGPUComputeRO_Buffers(SDL_GPUComputePass* cmp, uint32_t slot, const std::vector<BufferData*>& buffers_data, uint8_t frame);

	// Пишет в transfer-буфер самого таска (task->tbd) — одна функция на все фазы.
	void UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data);
	// ⚠️ ПО УМОЛЧАНИЮ НЕ ИСПОЛЬЗУЙ (в т.ч. нейросети): это перф-КОМПРОМИСС для ГОРЯЧЕГО пути с
	// большими объёмами (transform/instance/bound-sphere — тысячи-миллионы строк за кадр), где
	// лишний memcpy и промежуточный буфер реально стоят. Для обычной/редкой заливки бери
	// UploadToTransferBuffer — проще и безопаснее. Подводные камни прямого указателя: mapped-память
	// может быть write-combined (ТОЛЬКО писать, не читать — чтение медленное/некогерентное), и легко
	// ошибиться с размером/выравниванием (никто не проверит за тебя).
	//
	// Резервирует size байт в transfer-буфере таска (те же проверки и учёт written_size/
	// used_buffer_size, что у UploadToTransferBuffer) и возвращает указатель для прямой записи —
	// без промежуточного staging и лишнего memcpy. nullptr при ошибке.
	void* AcquireTransferWritePtr(UploadTask* task, Uint32 size);
	std::span<const std::byte> ReadFromTransferBuffer(ReadBackTask* task, uint32_t size);

	void TrashBuffers(uint64_t fences_done);

	BufferData* GetBufferData(BufferDataName name);
	// Весь реестр буферов — для UI (перечень для дропдаунов). Ключ карты (BufferDataName) и есть
	// каноничное имя буфера — его и показываем/кладём в ссылки sp (резолв назад через GetBufferData).
	const std::unordered_map<BufferDataName, std::unique_ptr<BufferData>>& GetBuffersData() const { return buffers_data; }
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
	// Обёртки, ждущие создания GPU-буфера. НЕвладеющие (владелец — buffers_data); буферы не
	// удаляются поимённо, так что висячих указателей тут не бывает. Дренируется BakePending.
	std::vector<BufferData*> pending_bakes;
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

