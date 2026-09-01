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
#include "config.h"   // BUFFERING_LEVEL — размер пер-слотового штампа регионов

struct Material;
namespace RenderSnap { struct BatchLayout; }

// ── Регионы индиректа и out_pib ──
// Оба буфера нарезаны на регион НА ПРОХОД, в регионе — блок на каждый ДРОУ прохода за кадр:
//
//   индирект: [ проход0: blocks0 x commands0 ][ проход1: blocks1 x commands1 ]…
//   out_pib:  [ проход0: blocks0 x pib0      ][ проход1: blocks1 x pib1      ]…
//
// Сколько блоков нужно проходу — говорит ОН САМ (инструкция счёта, см.
// CreateRegionCountInstruction), и это единственное, что он может сказать: границы он назвать
// не в состоянии. Расставляет регионы PassManager — владелец проходов и их порядка. Отсюда и
// согласованность: чей бы ни был счётчик и что бы он ни вернул, покрытие остаётся
// непересекающимся, а все потребители читают ОДИН штамп слота.
struct PassRegion {
    uint32_t command_blocks_count = 0;   // блоков (дроу прохода за кадр); 0 — региона нет
    uint32_t commands = 0;               // команд на блок (= PassDrawList::num_commands)
    uint32_t pib = 0;                    // PIB-записей на блок (= PassDrawList::num_instances)
    uint32_t first_pib = 0;              // начало сегмента прохода во ВХОДНОМ PIB
    uint32_t cmd_base = 0;               // база региона в индиректе, в командах
    uint32_t pib_base = 0;               // база региона в out_pib, в записях
};

struct PassRegions {
    std::vector<PassRegion> per_pass;   // индекс = ordinal прохода
    uint32_t total_commands = 0;        // сумма blocks*commands — размер индиректа в командах
    uint32_t total_pib = 0;             // сумма blocks*pib — размер out_pib в записях
};

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
	// region_index — какой ИЗ СВОИХ блоков рисуем (проход, рисуемый один раз, не передаёт
	// ничего). Байтовое смещение в индиректе считается здесь по штампу регионов слота —
	// наружу оно не отдаётся, чтобы не было второго способа его посчитать.
	void RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass, BufferManager* bm, uint32_t region_index, const void* push_data_raw);
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

	// Сколько блоков (дроу за кадр) нужно проходу в слоте. Регистрируется РЯДОМ с созданием
	// прохода — тем же приёмом, что resize-инструкции атласов и push/dispatch программ.
	// Функция обязана читать слепки слота (Ask*), а не живое состояние; вернуть она может
	// только ЧИСЛО — границы регионов расставит StampRegions, и разъехаться они не могут.
	// Нет инструкции = один блок (обычный проход, рисуемый один раз).
	void CreateRegionCountInstruction(const RenderPassName& name, std::function<uint32_t(uint8_t)> fn);

	// Расставляет регионы слота по слепку его раскладки. Зовётся ОДИН раз за prepare, из фазы
	// слепков Engine::PrepareFunc — после слепков, которые читают счётчики, и до инструкций
	// заливки, чьи size-функции спрашивают размеры буферов у AskRegions.
	void StampRegions(uint8_t slot, const RenderSnap::BatchLayout* layout);
	const PassRegions& AskRegions(uint8_t slot) const { return regions[slot]; }
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

	// Инструкции счёта регионов по имени прохода + пер-слотовый штамп раскладки регионов.
	std::unordered_map<RenderPassName, std::function<uint32_t(uint8_t)>> region_count_instructions;
	PassRegions regions[BUFFERING_LEVEL];

	bool passes_filled = false;
};