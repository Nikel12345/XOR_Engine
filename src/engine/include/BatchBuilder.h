#pragma once

class ObjectManager;
class PipeManager;
class PassManager;
class TextureManager;
class ShaderManager;
struct SceneData;

using Entity = uint32_t;

class BatchBuilder {
public:
	BatchBuilder();
	bool BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene);
	void BuildComputeBatches(PipeManager* pm, ShaderManager* sm);
	void RecalculateBatches(PipeManager* pm, PassManager* pass_manager, std::vector<Entity>& entites);
	void BuildComputePrepassBatches(PipeManager* pm, ShaderManager* sm);
	bool CheckPIBNeedUpload() { return need_PIB_upload; };
	void SetDirtyBatches(bool state) { dirty_batches = state; };
	void SetPIBNeedUpload(bool state) { need_PIB_upload = state; };
	uint32_t AskNumCommands();
private:
	std::vector<Entity> entities_to_create;
	std::vector<Entity> entities_to_delete;
	void FinilizeRenderBatches(PassManager* pass_manager);
	uint32_t total_commands = 0;
	bool dirty_batches = true;
	bool need_PIB_upload = true;	
};