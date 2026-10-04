#pragma once
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "config.h"
#include "ShaderManager.h"

class PassManager;

// Шейдер в ключе — данными (VSD/FSD), а не SDL_GPUShader: GPU-шейдер общий у вершинников с одним
// байткодом, а раскладка вершин у них разная. Пара «id + адрес» ломается, если в одну ячейку дважды
// положить новый объект между пересборками словаря (WARNINGS.md).
struct GraphicsPipelineKey {
	VertexShaderId            vs_id;
	const VertexShaderData*   vsd = nullptr;
	FragmentShaderId          fs_id;
	const FragmentShaderData* fsd = nullptr;
	ShaderProgramDescription  spd;
	std::vector<SDL_GPUTextureFormat> color_formats;
	SDL_GPUTextureFormat      depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;

	bool operator==(const GraphicsPipelineKey&) const = default;
};

struct GraphicsPipelineKeyHash {
	size_t operator()(const GraphicsPipelineKey& k) const;
};

class PipeManager
{
public:
	PipeManager(SDL_GPUDevice* device, SDL_Window* win);
	void CreateGraphicsPiplenes(const ShaderProgramRegistry& shader_programs, ShaderManager* sm, PassManager* pass_manager);
	void CreateComputePipelines(ComputeProgramRegistry& compute_shader_programs, ShaderManager* sm);

	SDL_GPUColorTargetDescription MakeNoColorTarget();

	std::shared_ptr<SDL_GPUGraphicsPipeline> FindGraphicsPipeline(const ShaderProgram& sp, ShaderManager* sm, PassManager* pass_manager) const;
	std::shared_ptr<SDL_GPUComputePipeline> GetComputePipeline(ComputeShaderProgram* sp);

	SDL_GPUDepthStencilTargetInfo depthTargetInfo{};

private:
	std::optional<GraphicsPipelineKey> MakeGraphicsPipelineKey(const ShaderProgram& sp, ShaderManager* sm, PassManager* pass_manager) const;
	std::shared_ptr<SDL_GPUGraphicsPipeline> CreateGraphicsPipeline(const GraphicsPipelineKey& key);
	std::shared_ptr<SDL_GPUComputePipeline> GetOrCreateComputePipeline(ComputeShaderProgram* sp, ShaderManager* sm);

	std::unordered_map<GraphicsPipelineKey, std::shared_ptr<SDL_GPUGraphicsPipeline>, GraphicsPipelineKeyHash> graphics_pipelines;

	SDL_Window* win;
	SDL_GPUDevice* dev;

};

