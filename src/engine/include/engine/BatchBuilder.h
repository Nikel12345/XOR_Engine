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
struct MatSpLayout;
struct SubMeshData;
struct TextureHandle;
struct ShaderProgram;
struct ModelData;
struct Material;
struct BufferData;

struct SubMeshDraw {
    uint32_t index_count = 0;
    uint32_t index_offset = 0;
    uint32_t vertex_offset = 0;
};

// Сущности, которые в проходе рисуются одинаковым набором команд. Все листы группы делят её
// записи PIB: first_instance/num_instances у них общие.
struct InstanceGroup {
    uint32_t pib_first = 0;
    uint32_t size = 0;
    uint8_t  lod_count = 1;
    ModelId  root_model;
};

struct CommandBatch {
    uint32_t    instance_group_index = 0;
    uint8_t     level = 0;
    uint32_t    firstInstance = 0;
    uint32_t    instanceCount = 0;
    SubMeshDraw submesh;
};

struct TextureBatch {
    std::unordered_map<BatchKeys::CommandBatchKey, CommandBatch> command_batches;
    std::vector<UVL_Block> texture_uvl;
    VariantLayout variant_layout;
    std::shared_ptr<std::vector<uint8_t>> params;
    std::shared_ptr<const PushInstructions> push_instructions;
};

struct GpuResourceBatch {
    std::unordered_map<BatchKeys::TextureBatchKey, TextureBatch> texture_batches;
    std::vector<BufferData*> vertexStorageBuffers;
    std::vector<BufferData*> fragmentStorageBuffers;
    std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
};

struct ShaderBatch {
    std::unordered_map<BatchKeys::GpuResourceBatchKey, GpuResourceBatch> resource_batches;
    std::vector<BufferData*> vertexBuffers;
    BufferData* indexBuffer = nullptr;
    std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
};

struct PassBatches {
    std::unordered_map<BatchKeys::ShaderBatchKey, ShaderBatch> shader_batches;
    std::vector<InstanceGroup> instance_groups;
};

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

	const std::vector<PassBatches>& GetPassBatches() const { return pass_batches; }

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
		uint32_t           pass = 0;
		TextureBatch*      texture = nullptr;
		const SubMeshData* submesh = nullptr;
		uint64_t           key = 0;
		uint8_t            level = 0;
	};
	std::vector<ResolvedCmd> resolved_scratch;

	struct ResolvedGroup {
		uint32_t         pass = 0;
		uint64_t         key = 0;
		uint8_t          lod_count = 1;
		ModelId          root_model;
	};
	std::vector<ResolvedGroup> group_scratch;

	void BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, TextureManager* tm,
		ShaderManager* sm, BufferManager* bm, ModelManager* mdm, MaterialManager* mtm, const DrawPopulation& population);
	void ApplyPopulation(const std::vector<GroupRange>& ranges);
	void BuildLayoutSnapshot(PassManager* pass_manager, BufferManager* bm);

	MatSpLayout BuildMatSpLayout(PipeManager* pm, PassManager* pass_manager, TextureManager* tm,
		ShaderManager* sm, const Material& material, const ShaderProgram& sp, ShaderProgramId sp_id,
		const std::shared_ptr<std::vector<uint8_t>>& sp_params) const;

	void ResolveRecipe(PipeManager* pm, PassManager* pass_manager, TextureManager* tm, ShaderManager* sm,
		BufferManager* bm, ModelManager* mdm, MaterialManager* mtm, const Recipe& recipe);
	void InsertResolvedRecipe(uint32_t recipe_index);
	TextureBatch* ResolveTextureBatch(PassBatches& pass, ShaderProgram* sp, const ShaderName& sp_name,
		const std::shared_ptr<std::vector<uint8_t>>& sp_params, const MatSpLayout& lay, uint32_t section,
		ShaderManager* sm, BufferManager* bm, uint64_t& path_key);

	TextureId dummy_texture;
	ShaderProgramId fallback_sp;

	std::vector<PassBatches> pass_batches;
	InstanceGroupTable group_table;
	std::vector<std::unordered_map<BatchKeys::InstanceGroupKey, uint32_t>> group_index_by_key;

	std::shared_ptr<const RenderSnap::BatchLayout> current_layout;
	std::shared_ptr<const RenderSnap::BatchLayout> slot_layouts[BUFFERING_LEVEL];

	std::shared_ptr<const RenderSnap::ComputeLayout> current_compute_layout;
	std::shared_ptr<const RenderSnap::ComputeLayout> slot_compute_layouts[BUFFERING_LEVEL];

	uint64_t batches_revision = 0;
	uint64_t rebuild_epoch = 0;
	std::atomic<bool> dirty_batches{ true };
};
