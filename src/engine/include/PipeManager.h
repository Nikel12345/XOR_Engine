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
	void CreateGraphicsPiplenes(std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& shader_programs, ShaderManager* sm);
	void CreateComputePipelines(std::vector<std::unique_ptr<ComputeShaderProgram>>& compute_shader_programs, ShaderManager* sm);

	SDL_GPUColorTargetDescription MakeDefaultColorTarget();
	SDL_GPUColorTargetDescription MakeNoColorTarget();

	SDL_GPUGraphicsPipeline* GetGraphicPipeline(ShaderProgram* sp);
	SDL_GPUComputePipeline* GetComputePipeline(ComputeShaderProgram* sp);

	// Сброс кэшированного графического пайплайна sp (правка spd/vs/fs или удаление sp). Старый
	// объект уходит в ОТЛОЖЕННОЕ удаление, из кэша убирается сразу; следующий CreateGraphicsPiplenes
	// пересоздаст. epoch — RebuildEpoch() на момент инвалидации: released только после того, как
	// планка эпох рендера уйдёт дальше (слепки старых слотов с этим указателем больше не показываются)
	// + дренаж BUFFERING_LEVEL render-fence (см. TrashPipelines).
	void InvalidatePipeline(ShaderProgram* sp, uint64_t epoch) {
		auto it = graphics_pipelines.find(sp);
		if (it != graphics_pipelines.end()) { QueueDeletePipeline(it->second, epoch); graphics_pipelines.erase(it); }
	}
	void QueueDeletePipeline(SDL_GPUGraphicsPipeline* p, uint64_t epoch) {
		if (!p) return;   // очередь одно-поточная: и пуш (команды), и дренаж (prepare) — sim
		pipeline_trash.push_back({ p, epoch, 0 });
	}
	// Сброс кэшированного compute-пайплайна csp (правка CSD). Тоже отложенно: рендер биндит его из
	// живого compute-дерева, поэтому гейт — эпоха пересборок этого дерева (ComputeRebuildEpoch).
	void InvalidateComputePipeline(ComputeShaderProgram* sp, uint64_t compute_epoch) {
		auto it = compute_pipelines.find(sp);
		if (it == compute_pipelines.end()) return;
		if (it->second) compute_trash.push_back({ it->second, compute_epoch, 0 });
		compute_pipelines.erase(it);
	}
	// Релиз пайплайнов из трэша. Дренаж — SIM в PrepareFunc (FenceThread лишь тикает счётчик
	// render-fence). Двухфазный гейт на запись: (1) армирование — соответствующая эпоха (планка
	// слотов / эпоха compute-дерева) ушла ДАЛЬШЕ эпохи инвалидации, с этого момента новые кадры
	// старый указатель не биндят; (2) стамп fences_done + BUFFERING_LEVEL — дренаж уже отправленных
	// на GPU. Чистый счётчик кадров (как было) освобождал пайплайн, пока устаревший слот ещё
	// показывался (sim не успел пере-prepare) → Invalid VkPipeline в валидаторе.
	void TrashPipelines(uint64_t fences_done, uint64_t graphics_required_epoch, uint64_t compute_rebuild_epoch);

	//void BindComputePipelines(ShaderManager* sm);
	SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
	~PipeManager();

private:
	SDL_GPUGraphicsPipeline* GetOrCreatePipeline(ShaderProgram* sp, ShaderManager* sm);
	SDL_GPUComputePipeline* GetOrCreateComputePipeline(ComputeShaderProgram* sp, ShaderManager* sm);

	std::unordered_map<ShaderProgram*, SDL_GPUGraphicsPipeline*> graphics_pipelines;
	std::unordered_map<ComputeShaderProgram*, SDL_GPUComputePipeline*> compute_pipelines;

	// epoch — эпоха на момент инвалидации (армирование: текущая эпоха > epoch); ready_at — стамп
	// освобождения (0 = не проставлен), см. TrashPipelines. Очереди ОДНО-поточные (sim), без замков.
	struct PendingPipelineDestroy { SDL_GPUGraphicsPipeline* pipe; uint64_t epoch; uint64_t ready_at; };
	struct PendingComputeDestroy  { SDL_GPUComputePipeline*  pipe; uint64_t epoch; uint64_t ready_at; };
	std::deque<PendingPipelineDestroy> pipeline_trash;   // отложенное удаление пайплайнов
	std::deque<PendingComputeDestroy>  compute_trash;

	SDL_Window* win;
	SDL_GPUDevice* dev;

};

