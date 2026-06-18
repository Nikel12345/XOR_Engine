#include "PCH.h"
#include "Engine.h"
#include "TexturesPresets.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

//void Engine::Iterate()
//{
//	Uint64 now = SDL_GetTicks();
//	double elapsed = (double)(now - prevTicks);
//	if (elapsed < frameTime) {
//		SDL_Delay((Uint32)(frameTime - elapsed));
//		now = SDL_GetTicks();
//	}
//	prevTicks = now; 
//
//	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
//
//	Uint32 w = 0, h = 0;
//	SDL_GPUTexture* tex = nullptr;
//
//	if (!SDL_AcquireGPUSwapchainTexture(cb, win, &tex, &w, &h)) {
//		SDL_Log("Acquire error: %s", SDL_GetError());
//		SDL_CancelGPUCommandBuffer(cb);
//		return;
//	}
//	if (!tex) {
//		SDL_CancelGPUCommandBuffer(cb);
//
//		return;
//	};
//
//	pass_manager->UpdateColorTargetInfoTex(tex);
//
//	buffer_manager->TrashBuffers();
//
//	SceneData* scene = object_manager->GetActiveScene();
//	buffer_manager->UpdateBuffer("cameraBuffer");
//	buffer_manager->UpdateBuffer("DefaultTransformBuffer", scene);
//	buffer_manager->UpdateBuffer("lightBuffer", &allLights);
//	buffer_manager->UpdateBuffer("DefaultPositionIndexBuffer");
//	model_manager->UploadModelBuffer(buffer_manager);
//	buffer_manager->_BuildUploadTasks();
//
//	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
//	ExecuteCP(cp);
//	SDL_EndGPUCopyPass(cp);
//
//	SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &pass_manager->colorTargetInfo, 1, &pipe_manager->depthTargetInfo);
//
//	pass_manager->ExecutePassesSteps(rp, buffer_manager, 0);
//	SDL_EndGPURenderPass(rp);
//
//	SDL_SubmitGPUCommandBuffer(cb);
//}

//void Engine::PrepareFunc(uint8_t slot)
//{
//	slot_controller->SetSlotState(slot, PREPARING);
//	buffer_manager->logic_index = slot;
//
//	pipe_manager->CreateGraphicsPiplenes(shader_manager);
//	pipe_manager->CreateComputePipelines(shader_manager);
//
//	batch_builder->BuildRenderBatches(pipe_manager, pass_manager, object_manager, object_manager->GetActiveScene());
//	batch_builder->BuildComputeBatches(pipe_manager, shader_manager);
//	batch_builder->BuildComputePrepassBatches(pipe_manager, shader_manager);
//
//	SDL_GPUCommandBuffer* cb0 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp0 = SDL_BeginGPUCopyPass(cb0);
//
//	buffer_manager->ExecutePrePassUpdateInstruction(cp0);
//	buffer_manager->ExecutePrePassUploadTasks(cp0, slot);
//
//	SDL_EndGPUCopyPass(cp0);
//
//	SDL_GPUFence* prepass_task_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb0);
//	SDL_WaitForGPUFences(dev, true, &prepass_task_fence, 1);
//	SDL_ReleaseGPUFence(dev, prepass_task_fence);
//
//	SDL_GPUCommandBuffer* cb1 = SDL_AcquireGPUCommandBuffer(dev);
//	pass_manager->ExecutePrepassesSteps(cb1, slot);
//	SDL_GPUFence* prepass_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb1);
//
//	SDL_WaitForGPUFences(dev, true, &prepass_fence, 1);
//	SDL_ReleaseGPUFence(dev, prepass_fence);
//
//	buffer_manager->MapUploadTransferBuffer();
//	buffer_manager->MapReadTransferBuffer();
//	texture_manager->MapUploadTransferBuffer();
//
//	// Полностью CPU логика
//	SDL_GPUCommandBuffer* cb2 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cb2);
//
//	buffer_manager->ExecuteReadBackInstructionsSize();
//
//	buffer_manager->ExecuteUpdateInstructions(cp2);
//	// Не неачинает загрузку до submit, всё ещё CPU
//	buffer_manager->ExecuteUploadTasks(cp2, slot);
//	texture_manager->ExecuteUploadTasks(cp2);
//	buffer_manager->ExecuteDownloadTasks(cp2, slot);
//
//	//SDL_WaitForGPUFences(dev, true, &prepass_fence, 1);
//	//SDL_ReleaseGPUFence(dev, prepass_fence);
//
//	SDL_EndGPUCopyPass(cp2);
//	SDL_GPUFence* upload_download_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb2);
//
//	buffer_manager->UnmapUploadTransferBuffer(); // Загрузок в обычном tb большей нет, анмапим
//	// buffer_manager->MapPostReadBackUploadTB() // Появится в будущем для post read back UI
//
//	SDL_WaitForGPUFences(dev, true, &upload_download_fence, 1);
//	SDL_ReleaseGPUFence(dev, upload_download_fence); // Ждём и завершения скачивания.
//
//
//	SDL_GPUCommandBuffer* cb3 = SDL_AcquireGPUCommandBuffer(dev);
//	SDL_GPUCopyPass* cp3 = SDL_BeginGPUCopyPass(cb3);
//
//	buffer_manager->ExecuteReadBackInstructionsReader();
//	// Потом появится тип задача со своим tb, которые для заливки данных в буфер тебуют завершения rb.
//	// buffer_manager->ExecutePostReadBackUpdateInstructions(cp3); // НУЖЕН CP3!
//	SDL_EndGPUCopyPass(cp3);
//
//	buffer_manager->UnmapReadTransferBuffer();
//	// buffer_manager->UnmapPostReadTransferBuffer();
//	texture_manager->UnmapUploadTransferBuffer();
//
//	texture_manager->GenerateMipmaps(cb3);
//
//	SDL_GPUFence* mip_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb3);
//	SDL_WaitForGPUFences(dev, true, &mip_fence, 1);
//	SDL_ReleaseGPUFence(dev, mip_fence);
//
//	slot_controller->SetSlotState(slot, PREPARED);
//	slot_controller->SetSlotState(slot, UPLOADING);
//	slot_controller->SetSlotState(slot, UPLOADED);
//}
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
		printf("  %-20s  spin  µs: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", label, sp.avg, sp.p50, sp.p95, sp.max);
		printf("  %-20s  kernel µs: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "", ke.avg, ke.p50, ke.p95, ke.max);
		printf("  %-20s  total  µs: avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "", to.avg, to.p50, to.p95, to.max);
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
		printf("\n┌── [%s]  %d samples ──────────────────────────────────────────\n", variant, n);
		fence_prepass_task.PrintRow("fence_prepass_task");
		printf("  ──────────────────────\n");
		if (prepass_eliminated)
			printf("  %-20s  [устранён — объединён в cb01]\n", "fence_prepass");
		else
			fence_prepass.PrintRow("fence_prepass");
		printf("  ──────────────────────\n");
		fence_download.PrintRow("fence_download");
		printf("  ──────────────────────\n");
		fence_postreadback.PrintRow("fence_postreadback");
		printf("  ══════════════════════\n");
		auto t = TotalAgg();
		printf("  %-20s         avg=%5lld  p50=%5lld  p95=%5lld  max=%6lld\n", "TOTAL FUNC µs:", t.avg, t.p50, t.p95, t.max);
		printf("└──────────────────────────────────────────────────────────────\n");
	}
};

static VariantStats g_stats_orig{ "ORIGINAL" };
static VariantStats g_stats_opt{ "OPTIMIZED" };
static int          g_stat_calls = 0;
static const int    PRINT_EVERY = 600;

void Engine::PrepareFunc(uint8_t slot)
{
	slot_controller->SetSlotState(slot, PREPARING);
	buffer_manager->logic_index = slot;

	engine_context->CreateGraphicsPipelines();
	engine_context->CreateComputePipelines();

	batch_builder->UpdateRenderBatches(pipe_manager, pass_manager, object_manager, object_manager->GetActiveScene());
	batch_builder->BuildComputeBatches(pass_manager, pipe_manager, shader_manager);
	PrepareFuncPrepassUndepended(slot);
	//PrepareFuncPrepassDepended(slot);
	//auto r1 = PrepareFuncPrepassDepended_Original(slot);
	//auto r2 = PrepareFuncPrepassDepended_Optimized(slot);
	//g_stats_orig.Add(r1);
	//g_stats_opt.Add(r2);
	//if (++g_stat_calls % PRINT_EVERY == 0) {
	//	g_stats_orig.Print();
	//	g_stats_opt.Print();
	//	auto o = g_stats_orig.TotalAgg();
	//	auto p = g_stats_opt.TotalAgg();
	//	printf("\n▶ avg: %+lld µs (%+.1f%%)   p95: %+lld µs (%+.1f%%)   [%d вызовов]\n",
	//		o.avg - p.avg, 100.0 * (o.avg - p.avg) / o.avg,
	//		o.p95 - p.p95, 100.0 * (o.p95 - p.p95) / o.p95,
	//		g_stat_calls);
	//}

	slot_controller->SetSlotState(slot, PREPARED);
	slot_controller->SetSlotState(slot, UPLOADING);
	slot_controller->SetSlotState(slot, UPLOADED);
}

void Engine::PrepareFuncPrepassUndepended(uint8_t slot)
{
	buffer_manager->MapUploadTransferBuffer();
	texture_manager->MapUploadTransferBuffer();

	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
	buffer_manager->ExecuteUpdateInstructions(cp);
	buffer_manager->ExecuteUploadTasks(cp, slot);
	texture_manager->ExecuteUploadTasks(cp);
	SDL_EndGPUCopyPass(cp);
	texture_manager->GenerateMipmaps(cb);
	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);

	SDL_WaitForGPUFences(dev, true, &fence, 1);
	SDL_ReleaseGPUFence(dev, fence);

	buffer_manager->UnmapUploadTransferBuffer();
	texture_manager->UnmapUploadTransferBuffer();
}

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

void Engine::PrepareFuncPrepassDepended(uint8_t slot)
{
	buffer_manager->MapPrepassDependedTransferBuffer();
	SDL_GPUCommandBuffer* cb0 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp0 = SDL_BeginGPUCopyPass(cb0);

	buffer_manager->ExecutePrePassUpdateInstruction(cp0);
	buffer_manager->ExecutePrePassUploadTasks(cp0, slot);

	SDL_EndGPUCopyPass(cp0);
	buffer_manager->UnmapPrepassDependedTransferBuffer();
	SDL_GPUFence* prepass_task_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb0);

	SDL_WaitForGPUFences(dev, true, &prepass_task_fence, 1);
	SDL_ReleaseGPUFence(dev, prepass_task_fence);


	SDL_GPUCommandBuffer* cb1 = SDL_AcquireGPUCommandBuffer(dev);

	pass_manager->ExecutePrepassesSteps(cb1, slot);

	SDL_GPUFence* prepass_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb1);
	SDL_WaitForGPUFences(dev, true, &prepass_fence, 1);
	SDL_ReleaseGPUFence(dev, prepass_fence);


	buffer_manager->MapReadTransferBuffer();
	SDL_GPUCommandBuffer* cb2 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cb2);

	buffer_manager->ExecuteReadBackInstructionsSize();
	buffer_manager->ExecuteDownloadTasks(cp2, slot);

	SDL_EndGPUCopyPass(cp2);
	SDL_GPUFence* download_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb2);
	SDL_WaitForGPUFences(dev, true, &download_fence, 1);
	SDL_ReleaseGPUFence(dev, download_fence);


	buffer_manager->ExecuteReadBackInstructionsReader();
	buffer_manager->UnmapReadTransferBuffer();


	buffer_manager->MapPrepassDependedTransferBuffer();
	SDL_GPUCommandBuffer* cb3 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp3 = SDL_BeginGPUCopyPass(cb3);
	buffer_manager->ExecutePostReadbackInstructions(cp3);
	buffer_manager->ExecutePostreadBackUploadTasks(cp3, slot);
	
	SDL_GPUFence* postreadbackUI_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb3);
	buffer_manager->UnmapPrepassDependedTransferBuffer();
	SDL_WaitForGPUFences(dev, true, &postreadbackUI_fence, 1);
	SDL_ReleaseGPUFence(dev, postreadbackUI_fence);
}

void Engine::UploadFunc(uint8_t slot)
{
	//slot_controller->SetSlotState(slot, UPLOADING);

	//SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
	//SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
	//buffer_manager->MapUploadTransferBuffer();
	//texture_manager->MapUploadTransferBuffer();
	//texture_manager->ExecuteUploadTasks(cp);
	//buffer_manager->UnmapUploadTransferBuffer();
	//texture_manager->UnmapUploadTransferBuffer();
	//SDL_EndGPUCopyPass(cp);
	//SDL_SubmitGPUCommandBuffer(cb);

	//slot_controller->SetSlotState(slot, UPLOADED);

}

bool Engine::RenderFunc(uint8_t slot)
{
	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);

	Uint32 w = 0, h = 0;
	SDL_GPUTexture* tex = nullptr;

	if (!SDL_AcquireGPUSwapchainTexture(cb, win, &tex, &w, &h)) {
		SDL_CancelGPUCommandBuffer(cb);
		return false;
	}
	if (!tex) {
		SDL_CancelGPUCommandBuffer(cb);
		return false;
	}

	pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::MAIN_PASS)->renderPassTexsData.SetColorTexture(tex);
	// Дебаг-пасс рисует поверх того же свопчейна (LOAD). Гард — пасс может быть не создан.
	if (RenderPassStep* debug_rp = pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::DEBUG_PASS))
		debug_rp->renderPassTexsData.SetColorTexture(tex);
	// Прозрачный пасс рисует в тот же свопчейн (LOAD). Гард — пасс может быть не создан.
	if (RenderPassStep* transparent_rp = pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::TRANSPARENT_PASS))
		transparent_rp->renderPassTexsData.SetColorTexture(tex);
	pass_manager->SetRenderFrame(slot);
	pass_manager->ExecutePassesSteps(cb, slot);

	BeginImGuiFrame();
	UI_ImGui::Iterate(engine_context);
	EndImGuiFrame();
	// === ImGui — только если данные готовы ===
	if (imgui_draw_data && imgui_draw_data->CmdListsCount > 0)
	{
		ImGui_ImplSDLGPU3_PrepareDrawData(imgui_draw_data, cb);

		SDL_GPUColorTargetInfo imgui_color_target = {};
		imgui_color_target.texture = tex;
		imgui_color_target.load_op = SDL_GPU_LOADOP_LOAD;
		imgui_color_target.store_op = SDL_GPU_STOREOP_STORE;
		imgui_color_target.cycle = false;

		SDL_GPURenderPass* imgui_rp = SDL_BeginGPURenderPass(cb, &imgui_color_target, 1, nullptr);
		ImGui_ImplSDLGPU3_RenderDrawData(imgui_draw_data, cb, imgui_rp);
		SDL_EndGPURenderPass(imgui_rp);
	}

	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
	slot_controller->SetSlotFence(slot, fence);
	slot_controller->SetSlotState(slot, RENDERING);

	return true;
}

void Engine::FenceFunc(uint8_t slot) {
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];
	SDL_GPUFence* fence = sd.fence;

	if (!fence) return;

	if (!SDL_QueryGPUFence(dev, fence))
		return;

	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;
	buffer_manager->TrashBuffers();

	//slot_controller->RemoveSlotFence(slot);
	slot_controller->SetSlotState(slot, RENDERED);

}

void Engine::BeginImGuiFrame()
{
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

void Engine::EndImGuiFrame()
{
	ImGui::Render();
	imgui_draw_data = ImGui::GetDrawData();
}

void Engine::OnWindowResized(Sint32 w, Sint32 h)
{
	width = safe_sint32_f(w);
	height = safe_sint32_f(h);
	auto tci = TexturePresets::GetCreateInfo(TexturePreset::SingleDepth2048);
	tci.width = safe_f_u32(width);
	tci.height = safe_f_u32(height);

	SDL_GPUTexture* new_depth_texture = texture_manager->CreateGPU_Texture(tci);

	//texture_manager->DeleteTexture(texture_manager->main_pass_depth_texture);
	texture_manager->main_pass_depth_texture = new_depth_texture;

	pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::MAIN_PASS)->renderPassTexsData.SetDepthTexture(texture_manager->main_pass_depth_texture);
	// Прозрачный пасс тестит ту же depth-текстуру MAIN_PASS — переназначаем после ресайза.
	if (RenderPassStep* transparent_rp = pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::TRANSPARENT_PASS))
		transparent_rp->renderPassTexsData.SetDepthTexture(texture_manager->main_pass_depth_texture);
}

Engine::Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height)
{
	this->win = window;
	this->dev = dev;
	this->width = width;
	this->height = height;
	buffer_manager = new BufferManager(dev);
	texture_manager = new TextureManager(dev);
	shader_manager = new ShaderManager(dev);
	pipe_manager = new PipeManager(dev, win);
	model_manager = new ModelManager();
	pass_manager = new PassManager();
	object_manager = new ObjectManager();
	camera_manager = new CameraManager();
	slot_controller = new SlotController();
	thread_controller = new ThreadController(slot_controller);
	material_manager = new MaterialManager();
	input_manager = new InputManager();
	texture_loader = new TextureLoader();

	batch_builder = new BatchBuilder();

	pib_data_module = new PIB_DataModule();
	transform_data_module = new TransformDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	bound_sphere_data_module = new BoundSphereDataModule();
	count_data_module = new CountBufferDataModule();

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	InitDefaultBufferUpdaters();
	InitPasses();
	InitUICommands();
	pass_manager->FillRenderPasses();

	thread_controller->SetPrepareCallback([this](uint8_t slot){this->PrepareFunc(slot);});
	thread_controller->SetUploadCallback([this](uint8_t slot) {this->UploadFunc(slot); });
	thread_controller->SetRenderCallback(
		[this](uint8_t slot) {
			return this->RenderFunc(slot);   // !!! return
		}
	);
	thread_controller->SetFenceCallback([this](uint8_t slot) {this->FenceFunc(slot); });

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = dev;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&init_info);
}

void Engine::InitDefaultBufferUpdaters()
{
	using namespace DefaultUpdateSet;

	SetDefaultCameraUpdater(*engine_context);
	SetDefaultPositionUpdater(*engine_context, transform_data_module);
	SetDefaultLightUpdater(*engine_context, light_data_module);
	SetDefaultPositionIndexUpdater(*engine_context, pib_data_module);
	SetDefaultVertexUpdater(*engine_context);
	SetDefaultIndexUpdater(*engine_context);
	SetDefaultLightCamerasUpdater(*engine_context, light_data_module);
	SetDefaultIndirectUpdater(*engine_context, indirect_data_module);

	//SetDefaultCountBufferUpdater(buffer_manager, object_manager, count_data_module, light_data_module, batch_builder);
	//SetDefaultBoundSphereUpdater(buffer_manager, pass_manager, model_manager, bound_sphere_data_module);
	//SetDefaultEntityToBatchUpdater(buffer_manager, object_manager, pass_manager, batch_builder, pib_data_module);
	//SetDefaultOutTransformUpdater(buffer_manager, transform_data_module);
	//SetDefaultOffsetBufferUpdater(buffer_manager, object_manager, count_data_module, light_data_module, batch_builder);
	//SetDefaultOutIndirectUpldater(buffer_manager, object_manager, batch_builder, light_data_module);
	//SetDefaultCountReader(buffer_manager, transform_data_module);

}

void Engine::InitPasses()
{
	using namespace DefaultRenderPassNamespace;
	//SetDefaultCullingComputeZerosPass(pass_manager, buffer_manager);
	//SetDefaultCullingComputeCountPass(pass_manager, buffer_manager, object_manager, transform_data_module, light_data_module, indirect_data_module);
	//SetDefaultCullingOutIndirectPass(pass_manager, buffer_manager);

	SetDefaultShadowPCFRenderPass(engine_context);
	//SetDefaultMainRenderPass(pass_manager, texture_manager, buffer_manager, dev, win);
	SetDefaultMainRenderPass(engine_context);
	SetTransparentPass(engine_context);     // прозрачная геометрия (блендинг) поверх сцены
	SetDebugColliderPass(engine_context);   // рамки коллайдеров поверх свопчейна
	//SetDefaultShadowVSMRenderPass(pass_manager, texture_manager, buffer_manager, object_manager, batch_builder);
	//SetDefaultShadowBlurPass(pass_manager, buffer_manager); // ДЛЯ VSM
	//SetDefaultMainRenderPass(pass_manager, texture_manager, buffer_manager);

	//SetDefaultCullingOffstPass(pass_manager, buffer_manager);
	//SetDefaultCullingOutTransformPass(pass_manager, buffer_manager, object_manager, transform_data_module, light_data_module, indirect_data_module);
}

void Engine::InitUICommands()
{
	input_manager->RegisterCommand(CommandId::DeleteEntity,
		[](EngineContext* ctx, const void* data)
		{
			Entity e = static_cast<Entity>(reinterpret_cast<uintptr_t>(data));
			ctx->DeleteEntity(ctx->GetObjectManager()->GetActiveSceneName(), e);
		});
}


Engine::~Engine()
{
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();

	delete buffer_manager;
	delete texture_manager;
	delete shader_manager;
	delete pipe_manager;
	delete model_manager;
	delete pass_manager;
	delete object_manager;
	delete camera_manager;
	delete slot_controller;
	delete thread_controller;
	delete material_manager;
	delete input_manager;
	delete texture_loader;
	delete pib_data_module;
	delete transform_data_module;
	delete light_data_module;

	dev = nullptr;
	win = nullptr;
}
