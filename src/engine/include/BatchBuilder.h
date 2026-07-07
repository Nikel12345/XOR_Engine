#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>

class ObjectManager;
class PipeManager;
class PassManager;
class TextureManager;
class ShaderManager;
struct SceneData;
struct ModelBatchData;
struct MaterialComponent;
struct ModelComponent;
struct TextureHandle;

using Entity = uint32_t;

class BatchBuilder {
public:
	BatchBuilder();
	// Brings the batch tree up to date: full rebuild when dirty, otherwise applies
	// the queued create/delete delta. Bumps the revision when the tree changed.
	void UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene);
	void BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm);
	void BuildComputePrepassBatches(PipeManager* pm, ShaderManager* sm);

	// Incremental delta (queued from UI thread, drained on prepare thread).
	void QueueCreate(Entity entity);
	void QueueDelete(Entity entity);

	// Monotonic version of the batch tree. Any consumer (PIB, indirect, ...) caches
	// the last revision it processed and re-uploads only when this differs — so the
	// engine never has to push dirtiness to a specific data module.
	uint64_t BatchesRevision() const { return batches_revision; }
	void SetDirtyBatches(bool state) { dirty_batches = state; };
	uint32_t AskNumCommands();
	// Число PIB-записей по всем пассам (сумма инстансов всех батчей). Нужен GPU-каллингу
	// (размер out_pib и диспатч), обновляется вместе с total_commands в FinalizeOffsets.
	uint32_t AskNumInstances();

	void SetDummyTexture(TextureHandle* dummy) { dummy_texture = dummy; };

private:
	// One place an entity lives inside a ModelBatchData::pib_sub_buffer.
	struct PibSlot {
		ModelBatchData* model_batch = nullptr;
		uint32_t        slot_index = 0; // index into model_batch->pib_sub_buffer
	};

	void BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene);
	// Drains the queues and applies them to the batch tree. Returns true if any
	// delta was applied.
	bool ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene);
	void FinalizeOffsets(PassManager* pass_manager);

	// Find-or-create the batch nodes for a single entity and record its slots.
	// Shared by full rebuild and incremental add.
	void AddEntityToBatches(Entity entity, PipeManager* pm,
		const MaterialComponent& material_component, const ModelComponent& model_component);
	void RemoveEntityFromBatches(Entity entity);

	TextureHandle* dummy_texture = nullptr;
	// Reverse index: entity -> all its slots across model batches. Rebuilt on full
	// rebuild, mutated incrementally by AddEntityToBatches/RemoveEntityFromBatches.
	std::unordered_map<Entity, std::vector<PibSlot>> entity_slots;

	std::mutex          delta_mutex;        // guards both delta queues
	std::vector<Entity> entities_to_create; // protected by delta_mutex
	std::vector<Entity> entities_to_delete; // protected by delta_mutex

	uint32_t total_commands = 0;
	uint32_t total_instances = 0;
	uint64_t batches_revision = 0;
	std::atomic<bool> dirty_batches{ true };
};
