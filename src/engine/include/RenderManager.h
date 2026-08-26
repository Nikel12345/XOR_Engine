#pragma once
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL.h>
#include <string_view>
#include <vector>
#include <variant>
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

	// Блит-проход: один блит src→dst, целиком данные (см. BlitPassStep). Свопчейн передаётся
	// как обычный атлас — GetSwapchainAtlas(); спецслучая в сигнатуре нет.
	// SDL требует у src SAMPLER, у dst COLOR_TARGET — оба атласа обязаны быть созданы с ними.
	// Prepass-варианта нет: блит вне проходов, а прекомпьют-cb существует ради scatter→indirect.
	BlitPassStep* CreateBlitPass(const BlitPassName& name, TextureAtlas* src, TextureAtlas* dst, int pass_index,
		SDL_GPUFilter filter = SDL_GPU_FILTER_NEAREST, SDL_GPULoadOp load_op = SDL_GPU_LOADOP_DONT_CARE);

	// Свопчейн-как-атлас. НЕ в реестре TextureManager: движок его не создаёт, не уничтожает,
	// не сэмплит и не селит в нём TextureHandle — регистрация потребовала бы гардов во всех
	// этих местах. Обёртка нужна ровно затем, чтобы блит-шаг держал единый TextureAtlas* и
	// не знал о спецслучае.
	// ПОТОКИ: пишет только render-поток (Engine::RenderFunc сразу после
	// SDL_AcquireGPUSwapchainTexture), читает он же — тело блита в том же вызове. Без замков.
	// Появится второй рендер-поток — это место придётся пересмотреть.
	void SetSwapchain(SDL_GPUTexture* tex, uint32_t w, uint32_t h);
	TextureAtlas* GetSwapchainAtlas() { return &swapchain_atlas; }
	void ResolveAllTextureTargets();

	void FillRenderPasses();
	void ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	void ExecutePrepassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame);
	// Начинает и завершает SDL_GPURenderPass
	// Starts and end SDL_GPURenderPass
	void RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw);
	// Начинает и завершает SDL_GPUComputePass
	// Starts and end SDL_GPUComputePass
	void ComputePassStandardBody(SDL_GPUCommandBuffer* cb, ComputePassStep* compute_pass, BufferManager* bm, const void* push_data_raw, const void* dispatch_data_raw, uint8_t pass_frame);
	// Единственное тело блит-прохода (функтора у него нет). Вне render/compute-пасса:
	// SDL_BlitGPUTexture запрещено звать внутри любого прохода.
	void BlitPassStandardBody(SDL_GPUCommandBuffer* cb, BlitPassStep& blit_pass);

	// Слот кадра + его слепок раскладки батчей (BatchBuilder::AskLayout(slot)) — ставится
	// Engine::RenderFunc перед записью команд. ExecuteRenderBatches рисует ТОЛЬКО по слепку:
	// живое дерево shader_batches рендер-поток не читает (оно приватно для sim).
	void SetRenderFrame(uint8_t frame, const RenderSnap::BatchLayout* layout) { render_frame = frame; render_layout = layout; }
	uint8_t RenderFrame() const { return render_frame; }
	RenderPassStep* GetRenderPassStep(const RenderPassName& name);
	ComputePassStep* GetComputePassStep(const ComputePassName& name);
	ComputePassStep* GetComputePrepassStep(const ComputePrepassName& name);
	BlitPassStep* GetBlitPassStep(const BlitPassName& name);

	// Обход отдаёт пару «имя + шаг»: имя прохода — это КЛЮЧ РЕЕСТРА, а не поле шага (debug_name
	// в логике не участвует — см. CLAUDE.md). Нужен редактору: он и перечисляет проходы, и
	// резолвит выбранный обратно. Порядок исполнения берут из pass_index самого шага.
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

	// Единый порядок исполнения кадра: render + compute + blit, слитые по pass_index (строит
	// FillRenderPasses). Раньше ExecutePassesSteps сливала два списка вручную двумя индексами —
	// с третьим видом прохода это стало бы нечитаемо. Вид прохода — variant (ровно один из трёх,
	// без «двух всегда мёртвых указателей»); pass_index продублирован рядом, чтобы сортировка
	// не лазила в variant.
	struct OrderedStep {
		int pass_index = -1;
		std::variant<RenderPassStep*, ComputePassStep*, BlitPassStep*> step;
	};
	std::vector<OrderedStep> ordered_execution;

	TextureAtlas swapchain_atlas{};   // см. SetSwapchain/GetSwapchainAtlas

	uint8_t render_frame = 0;
	const RenderSnap::BatchLayout* render_layout = nullptr;   // слепок раскладки рендеримого слота

	bool passes_filled = false;
};