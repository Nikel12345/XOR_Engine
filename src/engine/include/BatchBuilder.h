#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>
#include "config.h"
#include "Aliases.h"
#include "ResourceId.h"
#include "RenderCommandData.h"

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
	void UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	void BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm,
		BufferManager* bm, TextureManager* tm);

	void QueueCreate(Entity entity);
	void QueueDelete(Entity entity);
	// «Перевесить»: энтити жива, но её место в дереве изменилось (сменили модель или материал).
	// Отдельный путь, а не пара Delete+Create: у пары add-сторона гасится дважды — как «создана и
	// удалена в одном кадре» и гардом идемпотентности.
	void QueueUpdate(Entity entity);

	uint64_t BatchesRevision() const { return batches_revision; }
	uint64_t RebuildEpoch() const { return rebuild_epoch; }
	void SetDirtyBatches(bool state) { dirty_batches = state; };

	void StampLayoutSnapshot(uint8_t slot);
	// Счётчиков по всей раскладке здесь нет намеренно: размеры буферов, диспатчи и смещения дроу
	// считаются ПО ПРОХОДАМ (PassManager::StampRegions), а сумма по всем проходам ни для одного из
	// них не ответ.
	const RenderSnap::BatchLayout* AskLayout(uint8_t slot) const { return slot_layouts[slot].get(); }

	void SetDummyTexture(const std::string& name, TextureManager* tm);
	void SetFallbackShader(ShaderProgramId id) { fallback_sp = id; };

private:
	struct PibSlot {
		ModelBatchData* model_batch = nullptr;
		uint32_t        slot_index = 0;
	};

	void BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	bool ApplyIncremental(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, SceneData* scene);
	void FinalizeOffsets(PassManager* pass_manager, BufferManager* bm);

	// Памятка живёт до конца текущего UpdateRenderBatches и между вызовами НЕ валидна: протухнуть
	// она может от правки списка вариантов, переименования текстуры, правки sp у материала и репака
	// атласа. Точка инвалидации в движке одна — пересборка дерева; вторая рядом с ней разойдётся.
	void BuildMaterialLayouts(TextureManager* tm, ShaderManager* sm, MaterialManager* mtm);
	std::unordered_map<BatchKeys::MatSpKey, MatSpLayout> mat_sp_layouts;

	void AddEntityToBatches(Entity entity, PipeManager* pm, PassManager* pass_manager, TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm,
		const MaterialComponent& material_component, const ModelComponent& model_component);
	void RemoveEntityFromBatches(Entity entity);

	TextureId dummy_texture;
	ShaderProgramId fallback_sp;
	std::unordered_map<Entity, std::vector<PibSlot>> entity_slots;

	std::mutex          delta_mutex;
	std::vector<Entity> entities_to_create;
	std::vector<Entity> entities_to_delete;
	std::vector<Entity> entities_to_update;

	std::shared_ptr<const RenderSnap::BatchLayout> current_layout;
	std::shared_ptr<const RenderSnap::BatchLayout> slot_layouts[BUFFERING_LEVEL];

	uint64_t batches_revision = 0;
	uint64_t rebuild_epoch = 0;
	std::atomic<bool> dirty_batches{ true };
};
