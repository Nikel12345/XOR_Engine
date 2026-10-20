#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <string>
#include <cstdint>
#include "config.h"
#include "Aliases.h"
#include "ResourceId.h"
#include "RenderCommandData.h"
#include "DrawPopulation.h"

namespace RenderSnap { struct BatchLayout; struct ComputeLayout; }

class ObjectManager;
class PipeManager;
class PassManager;
class TextureManager;
class ShaderManager;
class BufferManager;
class ModelManager;
class MaterialManager;
struct SceneData;
struct TextureBatchData;
struct MatSpLayout;
struct RenderPassStep;
struct SubMeshData;
struct TextureHandle;
struct ShaderProgram;
struct ModelData;

class BatchBuilder {
public:
	BatchBuilder();
	void UpdateRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om,
		TextureManager* tm, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, DrawPopulation* population, SceneData* scene);
	void BuildComputeBatches(PassManager* pass_manager, PipeManager* pm, ShaderManager* sm,
		BufferManager* bm, TextureManager* tm);

	uint64_t BatchesRevision() const { return batches_revision; }
	uint64_t RebuildEpoch() const { return rebuild_epoch; }
	void SetDirtyBatches(bool state) { dirty_batches = state; };

	void StampLayoutSnapshot(uint8_t slot);
	// Счётчиков по всей раскладке здесь нет намеренно: размеры буферов, диспатчи и смещения дроу
	// считаются ПО ПРОХОДАМ (PassManager::StampRegions), а сумма по всем проходам ни для одного из
	// них не ответ.
	const RenderSnap::BatchLayout* AskLayout(uint8_t slot) const { return slot_layouts[slot].get(); }

	void StampComputeSnapshot(uint8_t slot);
	const RenderSnap::ComputeLayout* AskComputeLayout(uint8_t slot) const { return slot_compute_layouts[slot].get(); }

	void SetDummyTexture(const std::string& name, TextureManager* tm);
	void SetFallbackShader(ShaderProgramId id) { fallback_sp = id; };

private:
	struct ResolvedCmd {
		RenderPassStep*    pass = nullptr;
		TextureBatchData*  texture = nullptr;
		const SubMeshData* submesh = nullptr;
		uint64_t           key = 0;
		uint8_t            level = 0;
	};
	std::vector<ResolvedCmd> resolved_scratch;

	struct ResolvedGroup {
		RenderPassStep*  pass = nullptr;
		uint64_t         key = 0;
		uint8_t          lod_count = 1;
		const ModelData* root_model = nullptr;
	};
	std::vector<ResolvedGroup> group_scratch;

	void BuildRenderBatches(PassManager* pass_manager, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, const DrawPopulation& population);
	void FinalizeOffsets(PassManager* pass_manager, BufferManager* bm, const std::vector<GroupRange>& ranges);

	// Памятка живёт до конца текущего UpdateRenderBatches и между вызовами НЕ валидна: протухнуть
	// она может от правки списка вариантов, переименования текстуры, правки sp у материала и репака
	// атласа. Точка инвалидации в движке одна — пересборка дерева; вторая рядом с ней разойдётся.
	void BuildMaterialLayouts(PipeManager* pm, PassManager* pass_manager, TextureManager* tm,
		ShaderManager* sm, MaterialManager* mtm);
	std::unordered_map<BatchKeys::MatSpKey, MatSpLayout> mat_sp_layouts;

	void ResolveRecipe(PassManager* pass_manager, ShaderManager* sm, BufferManager* bm,
		ModelManager* mdm, MaterialManager* mtm, const Recipe& recipe);
	void InsertResolvedRecipe(uint32_t recipe_index);
	TextureBatchData* ResolveTextureBatch(RenderPassStep* rp, ShaderProgram* sp, const ShaderName& sp_name,
		const std::shared_ptr<std::vector<uint8_t>>& sp_params, const MatSpLayout& lay, uint32_t section,
		ShaderManager* sm, BufferManager* bm, uint64_t& path_key);

	TextureId dummy_texture;
	ShaderProgramId fallback_sp;

	DrawGroupTable group_table;
	std::vector<std::unordered_map<BatchKeys::GroupKey, uint32_t>> group_index_by_key;

	std::shared_ptr<const RenderSnap::BatchLayout> current_layout;
	std::shared_ptr<const RenderSnap::BatchLayout> slot_layouts[BUFFERING_LEVEL];

	std::shared_ptr<const RenderSnap::ComputeLayout> current_compute_layout;
	std::shared_ptr<const RenderSnap::ComputeLayout> slot_compute_layouts[BUFFERING_LEVEL];

	uint64_t batches_revision = 0;
	uint64_t rebuild_epoch = 0;
	std::atomic<bool> dirty_batches{ true };
};
