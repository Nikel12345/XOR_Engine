#pragma once
#include <unordered_map>
#include <deque>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "config.h"   // BUFFERING_LEVEL

class ShaderManager;
struct ShaderProgram;
struct ComputeShaderProgram;

class PipeManager
{
public:
	PipeManager(SDL_GPUDevice* device, SDL_Window* win);
	void CreateGraphicsPiplenes(std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& shader_programs);
	void CreateComputePipelines(std::vector<std::unique_ptr<ComputeShaderProgram>>& compute_shader_programs);

	SDL_GPUColorTargetDescription MakeDefaultColorTarget();
	SDL_GPUColorTargetDescription MakeNoColorTarget();

	SDL_GPUGraphicsPipeline* GetGraphicPipeline(ShaderProgram* sp);
	SDL_GPUComputePipeline* GetComputePipeline(ComputeShaderProgram* sp);

	// Сброс кэшированного графического пайплайна sp (правка spd / удаление sp). Старый объект уходит
	// в ОТЛОЖЕННОЕ удаление (может быть in-flight ещё BUFFERING_LEVEL кадров — тот же путь, что у
	// текстур/буферов), из кэша убирается сразу; следующий CreateGraphicsPiplenes пересоздаст из spd.
	void InvalidatePipeline(ShaderProgram* sp) {
		auto it = graphics_pipelines.find(sp);
		if (it != graphics_pipelines.end()) { QueueDeletePipeline(it->second); graphics_pipelines.erase(it); }
	}
	void QueueDeletePipeline(SDL_GPUGraphicsPipeline* p) { if (p) pipeline_trash.push_back({ p, BUFFERING_LEVEL }); }
	void TrashPipelines();   // релиз пайплайнов, переживших BUFFERING_LEVEL кадров (зовётся каждый кадр)

	//void BindComputePipelines(ShaderManager* sm);
	SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
	~PipeManager();

private:
	SDL_GPUGraphicsPipeline* GetOrCreatePipeline(ShaderProgram* sp);
	SDL_GPUComputePipeline* GetOrCreateComputePipeline(ComputeShaderProgram* sp);

	std::unordered_map<ShaderProgram*, SDL_GPUGraphicsPipeline*> graphics_pipelines;
	std::unordered_map<ComputeShaderProgram*, SDL_GPUComputePipeline*> compute_pipelines;

	struct PendingPipelineDestroy { SDL_GPUGraphicsPipeline* pipe; int frame_ready; };
	std::deque<PendingPipelineDestroy> pipeline_trash;   // отложенное удаление пайплайнов

	SDL_Window* win;
	SDL_GPUDevice* dev;

};

