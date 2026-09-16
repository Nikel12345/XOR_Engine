#pragma once
#include <unordered_map>
#include <memory>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "config.h"
#include "ShaderManager.h"

class PassManager;

class PipeManager
{
public:
	PipeManager(SDL_GPUDevice* device, SDL_Window* win);
	// pass_manager — резолвер прохода sp по имени (sp хранит имя, не указатель), тем же
	// параметром, что и ShaderManager: PipeManager чужих менеджеров не держит.
	void CreateGraphicsPiplenes(ShaderProgramRegistry& shader_programs, ShaderManager* sm, PassManager* pass_manager);
	void CreateComputePipelines(ComputeProgramRegistry& compute_shader_programs, ShaderManager* sm);

	SDL_GPUColorTargetDescription MakeDefaultColorTarget();
	SDL_GPUColorTargetDescription MakeNoColorTarget();

	std::shared_ptr<SDL_GPUGraphicsPipeline> GetGraphicPipeline(ShaderProgram* sp);
	std::shared_ptr<SDL_GPUComputePipeline> GetComputePipeline(ComputeShaderProgram* sp);

	SDL_GPUDepthStencilTargetInfo depthTargetInfo{};

private:
	std::shared_ptr<SDL_GPUGraphicsPipeline> GetOrCreatePipeline(ShaderProgram* sp, ShaderManager* sm, PassManager* pass_manager);
	std::shared_ptr<SDL_GPUComputePipeline> GetOrCreateComputePipeline(ComputeShaderProgram* sp, ShaderManager* sm);

	SDL_Window* win;
	SDL_GPUDevice* dev;

};

