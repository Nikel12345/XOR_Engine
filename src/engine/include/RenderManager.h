#pragma once
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL.h>
#include <string_view>
#include <vector>
//#include "ShaderManager.h"
#include "TextureData.h"
#include "RenderCommandData.h"
#include "Aliases.h"

struct Material;
namespace RenderSnap { struct BatchLayout; }

class MaterialManager;
class PipeManager;
class ObjectManager;
class BufferManager;   // только указатели в сигнатурах — полный тип тянут cpp-потребители

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
	// Начинает и завершает SDL_GPURenderPass
	// Starts and end SDL_GPURenderPass
	void RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw);
	void WaitComputePrepass(SDL_GPUDevice* dev);
	// Начинает и завершает SDL_GPUComputePass
	// Starts and end SDL_GPUComputePass
	void ComputePassStandardBody(SDL_GPUCommandBuffer* cb, ComputePassStep* compute_pass, BufferManager* bm, const void* push_data_raw, const void* dispatch_data_raw, uint8_t pass_frame);

	// Слот кадра + его слепок раскладки батчей (BatchBuilder::AskLayout(slot)) — ставится
	// Engine::RenderFunc перед записью команд. ExecuteRenderBatches рисует ТОЛЬКО по слепку:
	// живое дерево shader_batches рендер-поток не читает (оно приватно для sim).
	void SetRenderFrame(uint8_t frame, const RenderSnap::BatchLayout* layout) { render_frame = frame; render_layout = layout; }
	uint8_t RenderFrame() const { return render_frame; }
	RenderPassStep* GetRenderPassStep(const RenderPassName& name);
	ComputePassStep* GetComputePassStep(const ComputePassName& name);
	ComputePassStep* GetComputePrepassStep(const ComputePrepassName& name);

	const std::vector<RenderPassStep*>& GetOrderedRenderPasses() { return ordered_passes; }
	const std::vector<ComputePassStep*>& GetOrderedComputePasses() { return ordered_compute_steps; }
	const std::vector<ComputePassStep*>& GetOrderedComputePrepasses() { return ordered_compute_prepass_steps; }

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
	const RenderSnap::BatchLayout* render_layout = nullptr;   // слепок раскладки рендеримого слота

	bool passes_filled = false;
};