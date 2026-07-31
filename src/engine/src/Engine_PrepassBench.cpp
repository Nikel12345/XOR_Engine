#include "PCH.h"
#include "Engine.h"
#include "EngineProfiler.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "BufferManager.h"
#include "TransferManager.h"
#include "RenderManager.h"
#include "SlotController.h"

// ─────────────────────────────────────────────────────────────────────────────
//  A/B-СТЕНД префасса (PrepareFuncPrepassDepended): _Original vs _Optimized.
//  Вынесен из Engine.cpp, чтобы боевой файл держал ТОЛЬКО победившую версию.
//
//  Компилируется лишь при -DENGINE_BENCH=ON (option в engine/CMakeLists.txt).
//  Это не мусор: обе версии ожидания fence (SpinThenWait/PlainWait) и агрегатор
//  замеров живут здесь как КОМПИЛИРУЕМЫЙ код, а не комментарии — значит не сгниют
//  молча за `//`, когда вокруг поменяется API.
//
//  ВНИМАНИЕ: сам стенд ещё не завязан на текущий API — тела _Original/_Optimized
//  и прогон в живом PrepareFunc восстанавливает автор (перенос сделан "как есть").
// ─────────────────────────────────────────────────────────────────────────────
#if ENGINE_BENCH

struct FenceWaitResult {
	int64_t spin_us;
	int64_t kernel_us;
	int64_t total_us;
};

static FenceWaitResult SpinThenWait(SDL_GPUDevice* dev, SDL_GPUFence* fence)
{
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();
	while (!SDL_QueryGPUFence(dev, fence))
		_mm_pause();
	auto t1 = Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	auto t2 = Clock::now();
	return {
		std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
		std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count(),
		std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count()
	};
}

static FenceWaitResult PlainWait(SDL_GPUDevice* dev, SDL_GPUFence* fence)
{
	using Clock = std::chrono::steady_clock;
	auto t0 = Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	auto t1 = Clock::now();
	int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
	return { 0, us, us };
}

struct PrepassTimingReport {
	const char* variant;
	FenceWaitResult fence_prepass_task;
	FenceWaitResult fence_prepass;
	FenceWaitResult fence_download;
	FenceWaitResult fence_postreadback;
	int64_t total_func_us;
};

struct FenceStats {
	std::vector<int64_t> spin, kernel, total;

	void Add(const FenceWaitResult& r) {
		spin.push_back(r.spin_us);
		kernel.push_back(r.kernel_us);
		total.push_back(r.total_us);
	}

	struct Agg { int64_t avg, p50, p95, max; };
	static Agg Compute(std::vector<int64_t> v) {
		std::sort(v.begin(), v.end());
		int64_t sum = 0; for (auto x : v) sum += x;
		return { sum / (int64_t)v.size(), v[v.size() * 50 / 100], v[v.size() * 95 / 100], v.back() };
	}

	void PrintRow(const char* label) const {
		auto sp = Compute(spin);
		auto ke = Compute(kernel);
		auto to = Compute(total);
		printf("  %-20s  spin  us: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", label, sp.avg, sp.p50, sp.p95, sp.max);
		printf("  %-20s  kernel us: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "", ke.avg, ke.p50, ke.p95, ke.max);
		printf("  %-20s  total  us: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "", to.avg, to.p50, to.p95, to.max);
	}
};

struct VariantStats {
	const char* variant;
	bool prepass_eliminated = false;
	FenceStats fence_prepass_task, fence_prepass, fence_download, fence_postreadback;
	std::vector<int64_t> total_func;
	int n = 0;

	void Add(const PrepassTimingReport& r) {
		++n;
		fence_prepass_task.Add(r.fence_prepass_task);
		if (r.fence_prepass.total_us < 0)
			prepass_eliminated = true;
		else
			fence_prepass.Add(r.fence_prepass);
		fence_download.Add(r.fence_download);
		fence_postreadback.Add(r.fence_postreadback);
		total_func.push_back(r.total_func_us);
	}

	FenceStats::Agg TotalAgg() const { return FenceStats::Compute(total_func); }

	void Print() const {
		printf("\n== [%s]  %d samples ==========================================\n", variant, n);
		fence_prepass_task.PrintRow("fence_prepass_task");
		printf("  ----------------------\n");
		if (prepass_eliminated)
			printf("  %-20s  [eliminated - merged into cb01]\n", "fence_prepass");
		else
			fence_prepass.PrintRow("fence_prepass");
		printf("  ----------------------\n");
		fence_download.PrintRow("fence_download");
		printf("  ----------------------\n");
		fence_postreadback.PrintRow("fence_postreadback");
		printf("  ======================\n");
		auto t = TotalAgg();
		printf("  %-20s         avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "TOTAL FUNC us:", t.avg, t.p50, t.p95, t.max);
		printf("================================================================\n");
	}
};

static VariantStats g_stats_orig{ "ORIGINAL" };
static VariantStats g_stats_opt{ "OPTIMIZED" };
static int          g_stat_calls = 0;
static const int    PRINT_EVERY = 600;

// ─────────────────────────────────────────────────────────────────────────────
//  Тела A/B-вариантов префасса — перенесены из Engine.cpp КАК ЕСТЬ (закомментированы).
//  Чтобы стенд заработал: восстановить под текущий API и раскомментировать здесь.
//  Engine.h их уже объявляет (PrepareFuncPrepassDepended_Original/_Optimized).
// ─────────────────────────────────────────────────────────────────────────────
//PrepassTimingReport Engine::PrepareFuncPrepassDepended_Original(uint8_t slot)
//{
//	using Clock = std::chrono::steady_clock;
//	PrepassTimingReport r{ "ORIGINAL" };
//	auto t0 = Clock::now();
//
//	buffer_manager->MapPrepassDependedTransferBuffer();
//	SDL_GPUCommandBuffer* cb0 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp0 = SDL_BeginGPUCopyPass(cb0);
//	buffer_manager->ExecutePrePassUpdateInstruction(cp0);
//	buffer_manager->ExecutePrePassUploadTasks(cp0, slot);
//	SDL_EndGPUCopyPass(cp0);
//	buffer_manager->UnmapPrepassDependedTransferBuffer();
//	SDL_GPUFence* f0 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb0);
//	r.fence_prepass_task = PlainWait(dev, f0);
//	SDL_ReleaseGPUFence(dev, f0);
//
//	SDL_GPUCommandBuffer* cb1 = SDL_AcquireGPUCommandBuffer(dev);
//	pass_manager->ExecutePrepassesSteps(cb1, slot);
//	SDL_GPUFence* f1 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb1);
//	r.fence_prepass = PlainWait(dev, f1);
//	SDL_ReleaseGPUFence(dev, f1);
//
//	buffer_manager->MapReadTransferBuffer();
//	SDL_GPUCommandBuffer* cb2 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cb2);
//	buffer_manager->ExecuteReadBackInstructionsSize();
//	buffer_manager->ExecuteDownloadTasks(cp2, slot);
//	SDL_EndGPUCopyPass(cp2);
//	SDL_GPUFence* f2 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb2);
//	r.fence_download = PlainWait(dev, f2);
//	SDL_ReleaseGPUFence(dev, f2);
//	buffer_manager->ExecuteReadBackInstructionsReader();
//	buffer_manager->UnmapReadTransferBuffer();
//
//	buffer_manager->MapPrepassDependedTransferBuffer();
//	SDL_GPUCommandBuffer* cb3 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp3 = SDL_BeginGPUCopyPass(cb3);
//	buffer_manager->ExecutePostReadbackInstructions(cp3);
//	buffer_manager->ExecutePostreadBackUploadTasks(cp3, slot);
//	SDL_EndGPUCopyPass(cp3);
//	SDL_GPUFence* f3 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb3);
//	buffer_manager->UnmapPrepassDependedTransferBuffer();
//	r.fence_postreadback = PlainWait(dev, f3);
//	SDL_ReleaseGPUFence(dev, f3);
//
//	r.total_func_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
//	return r;
//}
//
//PrepassTimingReport Engine::PrepareFuncPrepassDepended_Optimized(uint8_t slot)
//{
//	using Clock = std::chrono::steady_clock;
//	PrepassTimingReport r{ "OPTIMIZED" };
//	r.fence_prepass = { -1, -1, -1 };
//	auto t0 = Clock::now();
//
//	buffer_manager->MapPrepassDependedTransferBuffer();
//	SDL_GPUCommandBuffer* cb01 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp0 = SDL_BeginGPUCopyPass(cb01);
//	buffer_manager->ExecutePrePassUpdateInstruction(cp0);
//	buffer_manager->ExecutePrePassUploadTasks(cp0, slot);
//	SDL_EndGPUCopyPass(cp0);
//	buffer_manager->UnmapPrepassDependedTransferBuffer();
//	pass_manager->ExecutePrepassesSteps(cb01, slot);
//	SDL_GPUFence* f01 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb01);
//	r.fence_prepass_task = SpinThenWait(dev, f01);
//	SDL_ReleaseGPUFence(dev, f01);
//
//	buffer_manager->MapReadTransferBuffer();
//	SDL_GPUCommandBuffer* cb2 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cb2);
//	buffer_manager->ExecuteReadBackInstructionsSize();
//	buffer_manager->ExecuteDownloadTasks(cp2, slot);
//	SDL_EndGPUCopyPass(cp2);
//	SDL_GPUFence* f2 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb2);
//	r.fence_download = SpinThenWait(dev, f2);
//	SDL_ReleaseGPUFence(dev, f2);
//	buffer_manager->ExecuteReadBackInstructionsReader();
//	buffer_manager->UnmapReadTransferBuffer();
//
//	buffer_manager->MapPrepassDependedTransferBuffer();
//	SDL_GPUCommandBuffer* cb3 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp3 = SDL_BeginGPUCopyPass(cb3);
//	buffer_manager->ExecutePostReadbackInstructions(cp3);
//	buffer_manager->ExecutePostreadBackUploadTasks(cp3, slot);
//	SDL_EndGPUCopyPass(cp3);
//	SDL_GPUFence* f3 = SDL_SubmitGPUCommandBufferAndAcquireFence(cb3);
//	buffer_manager->UnmapPrepassDependedTransferBuffer();
//	r.fence_postreadback = SpinThenWait(dev, f3);
//	SDL_ReleaseGPUFence(dev, f3);
//
//	r.total_func_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
//	return r;
//}

#endif // ENGINE_BENCH
