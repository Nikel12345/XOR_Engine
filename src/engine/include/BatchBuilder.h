#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>
#include "config.h"

namespace RenderSnap { struct BatchLayout; }

class ObjectManager;
class PipeManager;
class PassManager;
class TextureManager;
class ShaderManager;
class BufferManager;
class ModelManager;
class MaterialManager;
struct SceneData;
struct ModelBatchData;
struct MaterialComponent;
struct ModelComponent;
struct TextureHandle;
struct ShaderProgram;

using Entity = uint32_t;

class BatchBuilder {
public:
	BatchBuilder();
	// Brings the batch tree up to date: full rebuild when dirty, otherwise applies
	// the queued create/delete delta. Bumps the revision when the tree changed.
	// Резолверы name-based ссылок (имя модели/материала у энтити, имя текстуры/sp внутри материала
	// → указатель на сборке) передаются параметром, а не хранятся полем: системы движка не держат
	// указатели друг на друга.
	void UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	void BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm);
	void BuildComputePrepassBatches(PipeManager* pm, ShaderManager* sm);

	// Incremental delta (queued from UI thread, drained on prepare thread).
	void QueueCreate(Entity entity);
	void QueueDelete(Entity entity);
	// «Перевесить»: энтити ОСТАЁТСЯ, но её место в дереве изменилось (сменили модель/материал →
	// другие sp/atlas/model-батчи). Снимает со старых слотов и добавляет заново по ТЕКУЩИМ
	// компонентам — то же, что recreate, но без пересоздания самой энтити.
	// Отдельный путь, а не пара Delete+Create: у пары add-сторона гасится дважды — как «создана
	// и удалена в одном кадре» и гардом идемпотентности (энтити уже в дереве).
	void QueueUpdate(Entity entity);

	// Monotonic version of the batch tree. Any consumer (PIB, indirect, ...) caches
	// the last revision it processed and re-uploads only when this differs — so the
	// engine never has to push dirtiness to a specific data module.
	uint64_t BatchesRevision() const { return batches_revision; }
	// Эпоха ПОЛНЫХ пересборок дерева (в отличие от batches_revision, инкремент её НЕ двигает).
	// SlotController клеймит ею слоты: после редкого ребилда рендер держит кадр, пока
	// перестроенный слот не готов, вместо мерцания старой раскладкой indirect/out_pib.
	uint64_t RebuildEpoch() const { return rebuild_epoch; }
	// Эпоха пересборок COMPUTE-дерева (BuildComputeBatches). Атомик: пишет sim, читает
	// FenceThread — гейт отложенного удаления compute-пайплайнов (PipeManager::TrashPipelines):
	// после бампа дерево уже не держит старый указатель, остаётся дренаж in-flight кадров.
	uint64_t ComputeRebuildEpoch() const { return compute_rebuild_epoch.load(std::memory_order_acquire); }
	void SetDirtyBatches(bool state) { dirty_batches = state; };

	// ── Слепок раскладки батчей (RenderSnap::BatchLayout) ──
	// Пересобирается в FinalizeOffsets при изменении дерева; слоту присваивается в
	// StampLayoutSnapshot (PrepareFunc, ПЕРЕД заливкой буферов слота) — O(1), shared_ptr.
	// Ask*(slot) — ЕДИНСТВЕННЫЙ доступ рендера к раскладке: слепок слота гарантированно
	// совпадает с indirect_buffer[slot], живое дерево приватно для sim.
	void StampLayoutSnapshot(uint8_t slot);
	const RenderSnap::BatchLayout* AskLayout(uint8_t slot) const { return slot_layouts[slot].get(); }
	uint32_t AskNumCommands(uint8_t slot) const;
	// Число PIB-записей по всем пассам (сумма инстансов всех батчей) в раскладке слота.
	// Нужен GPU-каллингу (размер out_pib и диспатч) и адресации камерных блоков out_pib.
	uint32_t AskNumInstances(uint8_t slot) const;

	// Dummy-текстура ПО ИМЕНИ (та же конвенция, что SetFallbackShader ниже): резолв на сборке
	// батча, удаление dummy = промах → пропуск отрисовки sp у битого материала (пустой рендер,
	// без висячего указателя). Раньше держался сырой TextureHandle* — удаление dummy крашило.
	// tm — на вызове (не полем): dummy подставляется вместо отсутствующего слота материала, то есть
	// биндится ФРАГМЕНТНЫМ СЭМПЛЕРОМ. Материала у него нет, поэтому SAMPLER его атласу собирается
	// здесь — единственная декларация «эта текстура будет сэмплиться как фолбэк».
	void SetDummyTexture(const std::string& name, TextureManager* tm);
	// Fallback-sp ПО ИМЕНИ для материалов, чья sp удалена. Резолвится на сборке как обычная sp,
	// поэтому удаление самого fallback = промах → пустой рендер (без висячего указателя).
	void SetFallbackShader(const std::string& name) { fallback_shader_name = name; };

private:
	// One place an entity lives inside a ModelBatchData::pib_sub_buffer.
	struct PibSlot {
		ModelBatchData* model_batch = nullptr;
		uint32_t        slot_index = 0; // index into model_batch->pib_sub_buffer
	};

	void BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	// Drains the queues and applies them to the batch tree. Returns true if any
	// delta was applied.
	bool ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	void FinalizeOffsets(PassManager* pass_manager, BufferManager* bm);

	// Find-or-create the batch nodes for a single entity and record its slots.
	// Shared by full rebuild and incremental add.
	void AddEntityToBatches(Entity entity, PipeManager* pm, TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm,
		const MaterialComponent& material_component, const ModelComponent& model_component);
	void RemoveEntityFromBatches(Entity entity);

	std::string dummy_texture_name;     // dummy по имени для битых текстурных ссылок (см. SetDummyTexture)
	std::string fallback_shader_name;   // sp по имени для материала с удалённой sp (см. SetFallbackShader)
	// Reverse index: entity -> all its slots across model batches. Rebuilt on full
	// rebuild, mutated incrementally by AddEntityToBatches/RemoveEntityFromBatches.
	std::unordered_map<Entity, std::vector<PibSlot>> entity_slots;

	std::mutex          delta_mutex;        // guards all delta queues
	std::vector<Entity> entities_to_create; // protected by delta_mutex
	std::vector<Entity> entities_to_delete; // protected by delta_mutex
	std::vector<Entity> entities_to_update; // protected by delta_mutex

	// Текущая раскладка (двойник дерева после FinalizeOffsets) и её пер-слотовые стампы.
	// current_layout пишет только sim (prepare); слоты берут shared_ptr — старый слот
	// в полёте продолжает держать свою версию, пока не переиспользован.
	std::shared_ptr<const RenderSnap::BatchLayout> current_layout;
	std::shared_ptr<const RenderSnap::BatchLayout> slot_layouts[BUFFERING_LEVEL];

	uint64_t batches_revision = 0;
	uint64_t rebuild_epoch = 0;   // ++ только на полном ребилде (не на инкременте)
	std::atomic<uint64_t> compute_rebuild_epoch{ 0 };   // ++ в конце BuildComputeBatches (см. геттер)
	std::atomic<bool> dirty_batches{ true };
};
