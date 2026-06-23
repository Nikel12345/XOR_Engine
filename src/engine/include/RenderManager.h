#pragma once
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL.h>
#include <string_view>
#include <vector>
#include <mutex>
//#include "ShaderManager.h"
#include "TextureData.h"
#include "BufferManager.h"
#include "RenderCommandData.h"
#include "Aliases.h"

struct Material;

class MaterialManager;
class PipeManager;
class ObjectManager;

class PassManager
{
public:
    PassManager();
	RenderPassStep* CreateRenderPass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function, RenderPassTexturesInfo&& rptd, int pass_index);
	ComputePassStep* CreateComputePass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index);
	ComputePassStep* CreateComputePrepass(const ComputePrepassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index);

	void FillRenderPasses();
	void ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	void ExecutePrepassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	// �������� � ��������� SDL_GPURenderPass
	// Starts and end SDL_GPURenderPass
	void RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw);
	void WaitComputePrepass(SDL_GPUDevice* dev);
	// �������� � ��������� SDL_GPUComputePass
	// Starts and end SDL_GPUComputePass
	void ComputePassStandardBody(SDL_GPUCommandBuffer* cb, ComputePassStep* compute_pass, BufferManager* bm, const void* push_data_raw, const void* dispatch_data_raw, uint8_t pass_frame);

	void SetRenderFrame(uint8_t frame) { render_frame = frame; }
	RenderPassStep* GetRenderPassStep(const RenderPassName& name);
	ComputePassStep* GetComputePassStep(const ComputePassName& name);
	ComputePassStep* GetComputePrepassStep(const ComputePrepassName& name);

	const std::vector<RenderPassStep*>& GetOrderedRenderPasses() { return ordered_passes; }
	const std::vector<ComputePassStep*>& GetOrderedComputePasses() { return ordered_compute_steps; }
	const std::vector<ComputePassStep*>& GetOrderedComputePrepasses() { return ordered_compute_prepass_steps; }

	// Защищает СТРУКТУРУ дерева батчей (shader_batches в RenderPassStep) на время ПОЛНОЙ
	// пересборки: BuildRenderBatches делает shader_batches.clear() + перезаполнение, снося
	// узлы, по которым параллельно идёт render (ExecuteRenderBatches). Берётся в
	// BuildRenderBatches (prep-поток) и вокруг ExecutePassesSteps (render-поток).
	// Инкремент (count/offset → per-slot indirect) узлы не удаляет и замок НЕ берёт.
	// Ребилд редок (загрузка/смена сцены) — конкуренция минимальна.
	std::mutex& BatchTreeMutex() { return batch_tree_mutex_; }

	~PassManager();

private:
	void ExecuteRenderBatches(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* SDL_rp, const RenderPassStep& rp, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw);
	std::unordered_map<RenderPassName, std::unique_ptr<RenderPassStep>> render_steps;
	std::unordered_map<ComputePassName, std::unique_ptr<ComputePassStep>> compute_steps;
	std::unordered_map<ComputePrepassName, std::unique_ptr<ComputePassStep>> compute_prepass_steps;

	std::vector<RenderPassStep*> ordered_passes;
	std::vector<ComputePassStep*> ordered_compute_steps;
	std::vector<ComputePassStep*> ordered_compute_prepass_steps;

	uint8_t render_frame = 0;

	bool passes_filled = false;

	std::mutex batch_tree_mutex_;   // см. BatchTreeMutex() — замок на полную пересборку дерева
};