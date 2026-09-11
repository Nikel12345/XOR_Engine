#pragma once
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL.h>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>
#include <variant>
#include "TextureData.h"
#include "RenderCommandData.h"
#include "Aliases.h"
#include "config.h"

struct Material;
namespace RenderSnap { struct BatchLayout; }

struct PassRegion {
    uint32_t command_blocks_count = 0;
    uint32_t commands = 0;
    uint32_t pib = 0;
    uint32_t first_pib = 0;
    uint32_t cmd_base = 0;
    uint32_t pib_base = 0;
};

struct PassRegions {
    std::vector<PassRegion> per_pass;
    uint32_t total_commands = 0;
    uint32_t total_pib = 0;
};

class MaterialManager;
class PipeManager;
class ObjectManager;
class BufferManager;

class PassManager
{
public:
    PassManager();
	RenderPassStep* CreateRenderPass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function, RenderPassTexturesInfo&& rptd, int pass_index);
	ComputePassStep* CreateComputePass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index);
	ComputePassStep* CreateComputePrepass(const ComputePrepassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index);

	BlitPassStep* CreateBlitPass(const BlitPassName& name, TextureAtlas* src, TextureAtlas* dst, int pass_index,
		SDL_GPUFilter filter = SDL_GPU_FILTER_NEAREST, SDL_GPULoadOp load_op = SDL_GPU_LOADOP_DONT_CARE);

	void SetSwapchain(SDL_GPUTexture* tex, uint32_t w, uint32_t h);
	TextureAtlas* GetSwapchainAtlas() { return &swapchain_atlas; }
	void ResolveAllTextureTargets();

	void FillRenderPasses();
	void ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	void ExecutePrepassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	void RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass, BufferManager* bm, uint32_t region_index, const void* push_data_raw);
	void ComputePassStandardBody(SDL_GPUCommandBuffer* cb, ComputePassStep* compute_pass, BufferManager* bm, const void* push_data_raw, const void* dispatch_data_raw, uint8_t pass_frame);
	void BlitPassStandardBody(SDL_GPUCommandBuffer* cb, BlitPassStep& blit_pass);

	void SetRenderFrame(uint8_t frame, const RenderSnap::BatchLayout* layout) { render_frame = frame; render_layout = layout; }
	uint8_t RenderFrame() const { return render_frame; }

	void CreateRegionCountInstruction(const RenderPassName& name, std::function<uint32_t(uint8_t)> fn);

	void StampRegions(uint8_t slot, const RenderSnap::BatchLayout* layout);
	const PassRegions& AskRegions(uint8_t slot) const { return regions[slot]; }
	RenderPassStep* GetRenderPassStep(const RenderPassName& name);
	ComputePassStep* GetComputePassStep(const ComputePassName& name);
	ComputePassStep* GetComputePrepassStep(const ComputePrepassName& name);
	BlitPassStep* GetBlitPassStep(const BlitPassName& name);

	const std::unordered_map<RenderPassName, std::unique_ptr<RenderPassStep>>& GetRenderPasses() const { return render_steps; }
	const std::unordered_map<ComputePassName, std::unique_ptr<ComputePassStep>>& GetComputePasses() const { return compute_steps; }
	const std::unordered_map<ComputePrepassName, std::unique_ptr<ComputePassStep>>& GetComputePrepasses() const { return compute_prepass_steps; }

	const std::vector<RenderPassStep*>& GetOrderedRenderPasses() { return ordered_passes; }
	const std::vector<ComputePassStep*>& GetOrderedComputePasses() { return ordered_compute_steps; }
	const std::vector<ComputePassStep*>& GetOrderedComputePrepasses() { return ordered_compute_prepass_steps; }
	const std::vector<BlitPassStep*>& GetOrderedBlitPasses() { return ordered_blit_steps; }

	~PassManager();

private:
	void ExecuteRenderBatches(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* SDL_rp, const RenderPassStep& rp, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw);
	std::unordered_map<RenderPassName, std::unique_ptr<RenderPassStep>> render_steps;
	std::unordered_map<ComputePassName, std::unique_ptr<ComputePassStep>> compute_steps;
	std::unordered_map<ComputePrepassName, std::unique_ptr<ComputePassStep>> compute_prepass_steps;
	std::unordered_map<BlitPassName, std::unique_ptr<BlitPassStep>> blit_steps;

	std::vector<RenderPassStep*> ordered_passes;
	std::vector<ComputePassStep*> ordered_compute_steps;
	std::vector<ComputePassStep*> ordered_compute_prepass_steps;
	std::vector<BlitPassStep*> ordered_blit_steps;

	struct OrderedStep {
		int pass_index = -1;
		std::variant<RenderPassStep*, ComputePassStep*, BlitPassStep*> step;
	};
	std::vector<OrderedStep> ordered_execution;

	TextureAtlas swapchain_atlas{};

	uint8_t render_frame = 0;
	const RenderSnap::BatchLayout* render_layout = nullptr;

	std::unordered_map<RenderPassName, std::function<uint32_t(uint8_t)>> region_count_instructions;
	PassRegions regions[BUFFERING_LEVEL];

	bool passes_filled = false;
};