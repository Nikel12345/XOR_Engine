#include "PCH.h"
#include "QueueManager.h"
#include "Engine.h"
#include "EngineProfiler.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "PipeManager.h"
#include "ModelManager.h"   // ReclaimRanges — дренаж отложенных возвратов места в пулах
#include "TransferManager.h"
#include "SlotController.h"
#include "RenderManager.h"
#include "ObjectManager.h"
#include "BatchBuilder.h"
#include "EngineContext.h"
#include "UI_Yoga.h"
#include "CameraManager.h"
#include "CameraStruct.h"
#include "DefaultRenderPassSet.h"
#include "UI_ImGui.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

void Engine::PrepareFunc(uint8_t slot)
{
	buffer_manager->logic_index = slot;

	ui_yoga->Emit(engine_context, GetWidth(), GetHeight());

	if (Camera* cam = camera_manager->GetActiveCamera()) {
		const float h = GetHeight();
		if (h > 0.0f) cam->SetAspect(GetWidth() / h);
	}

	{
		const uint64_t fences_done = slot_controller->RenderFencesDone();
		buffer_manager->TrashBuffers(fences_done);
		pipe_manager->TrashPipelines(fences_done, slot_controller->RequiredEpoch(), batch_builder->ComputeRebuildEpoch());
		// Место снятых моделей — в разметку пула, пачкой. Фенса не ждёт (в отличие от двух строк
		// выше): освобождается разметка, а не GPU-ресурс — та же дисциплина, что у регионов
		// атласа. Обязано идти ДО размещения новой пачки, то есть до ExecuteUpdateInstructions.
		model_manager->ReclaimRanges();
	}

	{
		PROF_SCOPE(Sim, " bake_gpu_resources");
		buffer_manager->BakePending();
		texture_manager->BakePending();
	}

	{
		PROF_SCOPE(Sim, " pipelines (create g+c)");
		engine_context->CreateGraphicsPipelines();
		engine_context->CreateComputePipelines();
	}

	{
		PROF_SCOPE(Sim, " pack_atlases");
		texture_manager->PackAtlases();
	}

	{
		PROF_SCOPE(Sim, " update_render_batches");
		batch_builder->UpdateRenderBatches(pipe_manager, pass_manager, object_manager, texture_manager, shader_manager, buffer_manager,
			model_manager, material_manager, object_manager->GetActiveScene());
	}

	slot_controller->StampSlotEpoch(slot, batch_builder->RebuildEpoch());

	batch_builder->StampLayoutSnapshot(slot);

	{
		PROF_SCOPE(Sim, " build_compute_batches");
		batch_builder->BuildComputeBatches(pass_manager, pipe_manager, shader_manager, buffer_manager, texture_manager);
	}

	{
		PROF_SCOPE(Sim, " prepass_undepended (submit загрузки)");
		PrepareFuncPrepassUndepended(slot);
	}
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
}

void Engine::PrepareFuncPrepassUndepended(uint8_t slot)
{
	if (DISABLE_UPLOAD) {
		UploadCommandBuffer cb = queue_manager->GetUploadQueue().AcquireCommandBuffer();
		UploadCopyPass cp = cb.BeginBufferCopyPass();
		TransferBufferData* tbd;
		{
			PROF_SCOPE(Sim, "  exec_update_instructions (всего)");
			tbd = buffer_manager->ExecuteUpdateInstructions(cp.Raw());
		}
		cp.End();
		cb.Cancel();
		transfer_manager->ReleaseTB(tbd);
		// Текстуры дренируем и здесь: задачи ставятся в PackAtlases независимо от флага, и без
		// забора векторы задач растут без границ. Схема та же, что у буферов выше — команды
		// пишем, cb отменяем, TB возвращаем сразу (fence не нужен).
		{
			RenderCommandBuffer tex_cb = queue_manager->GetRenderQueue().AcquireCommandBuffer();
			TextureCopyPass tex_cp = tex_cb.BeginTextureCopyPass();
			TransferBufferData* tex_tbd = texture_manager->ExecuteUploadTasks(tex_cp.Raw());
			tex_cp.End();
			texture_manager->GenerateMipmaps(tex_cb.Raw());
			texture_manager->BlitPendingPreviews(tex_cb.Raw());
			tex_cb.Cancel();
			transfer_manager->ReleaseTB(tex_tbd);
		}
		slot_controller->SetSlotState(slot, SlotState::UPLOADING);
		slot_controller->SetSlotState(slot, SlotState::PREPARED);
		return;
	}

	UploadCommandBuffer upload_cb = queue_manager->GetUploadQueue().AcquireCommandBuffer();
	UploadCopyPass upload_cp = upload_cb.BeginBufferCopyPass();

	TransferBufferData* undepended_tbd;
	{
		PROF_SCOPE(Sim, "  exec_update_instructions (всего)");
		pending_upload_tbs[slot] = buffer_manager->ExecuteUpdateInstructions(upload_cp.Raw());
	}
	{
		PROF_SCOPE(Sim, "  exec_upload_tasks (буферы)");
		buffer_manager->ExecuteUploadTasks(upload_cp.Raw(), slot);
		upload_cp.End();

		slot_controller->PushUploadFence(slot, upload_cb.SubmitAndAcquireFence());
	}

	{
		PROF_SCOPE(Sim, "  tex_upload (upload+mips+preview)");
		RenderCommandBuffer tex_cb = queue_manager->GetRenderQueue().AcquireCommandBuffer();
		TextureCopyPass tex_cp = tex_cb.BeginTextureCopyPass();
		pending_texture_tbs[slot] = texture_manager->ExecuteUploadTasks(tex_cp.Raw());
		tex_cp.End();
		texture_manager->GenerateMipmaps(tex_cb.Raw());
		texture_manager->BlitPendingPreviews(tex_cb.Raw());

		slot_controller->PushUploadFence(slot, tex_cb.SubmitAndAcquireFence());
	}

	slot_controller->GetSlotsData()[slot].upload.submit_time = Prof::Clock::now();
	slot_controller->SetSlotState(slot, SlotState::UPLOADING);
}

void Engine::UploadFunc(uint8_t slot)
{
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];

	if (sd.upload.Empty()) return;

	auto t_wait = Prof::Clock::now();
	SDL_WaitForGPUFences(dev, true, sd.upload.items, sd.upload.count);
	double wait_ms = Prof::MsSince(t_wait);
	double upload_ms = Prof::MsSince(sd.upload.submit_time);
	for (uint8_t i = 0; i < sd.upload.count; ++i)
		SDL_ReleaseGPUFence(dev, sd.upload.items[i]);
	sd.upload.Clear();

	// Оба TB стадии: буферный (копировальная очередь) и текстурный (графическая). Fences обоих
	// только что отработали общим wait_all, значит GPU дочитал и тот и другой.
	auto t_rel = Prof::Clock::now();
	transfer_manager->ReleaseTB(pending_upload_tbs[slot]);
	pending_upload_tbs[slot] = nullptr;
	transfer_manager->ReleaseTB(pending_texture_tbs[slot]);
	pending_texture_tbs[slot] = nullptr;
	double release_ms = Prof::MsSince(t_rel);

	slot_controller->SetSlotState(slot, SlotState::PREPARED);

	Prof::Upload().Add("upload_gpu (submit->fence, заливка на GPU)", upload_ms);
	Prof::Upload().Add("upload_fence_wait (CPU-блок)", wait_ms);
	Prof::Upload().Add("release_tbs (возврат TB в пул)", release_ms);
	PROF_FRAME(Upload);
}

void Engine::ComputeFunc(uint8_t slot)
{
	slot_controller->SetSlotState(slot, SlotState::COMPUTING);

	{
		ComputeCommandBuffer ccb = queue_manager->GetComputeQueue().AcquireCommandBuffer();
		pass_manager->ExecutePrepassesSteps(ccb.Raw(), slot);
		SDL_GPUFence* cull_fence = ccb.SubmitAndAcquireFence();
		SDL_WaitForGPUFences(dev, true, &cull_fence, 1);
		SDL_ReleaseGPUFence(dev, cull_fence);
	}

	slot_controller->SetSlotState(slot, SlotState::COMPUTED);
}

void Engine::PrepareFuncPrepassDepended(uint8_t slot)
{
	SDL_GPUCommandBuffer* cb0 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp0 = SDL_BeginGPUCopyPass(cb0);

	TransferBufferData* prepass_tbd = buffer_manager->ExecutePrePassUpdateInstruction(cp0);
	buffer_manager->ExecutePrePassUploadTasks(cp0, slot);

	SDL_EndGPUCopyPass(cp0);
	SDL_GPUFence* prepass_task_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb0);

	SDL_WaitForGPUFences(dev, true, &prepass_task_fence, 1);
	SDL_ReleaseGPUFence(dev, prepass_task_fence);
	transfer_manager->ReleaseTB(prepass_tbd);


	SDL_GPUCommandBuffer* cb1 = SDL_AcquireGPUCommandBuffer(dev);

	pass_manager->ExecutePrepassesSteps(cb1, slot);

	SDL_GPUFence* prepass_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb1);
	SDL_WaitForGPUFences(dev, true, &prepass_fence, 1);
	SDL_ReleaseGPUFence(dev, prepass_fence);


	SDL_GPUCommandBuffer* cb2 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cb2);

	TransferBufferData* readback_tbd = buffer_manager->ExecuteReadBackInstructionsSize();
	buffer_manager->ExecuteDownloadTasks(cp2, slot);

	SDL_EndGPUCopyPass(cp2);
	SDL_GPUFence* download_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb2);
	SDL_WaitForGPUFences(dev, true, &download_fence, 1);
	SDL_ReleaseGPUFence(dev, download_fence);


	buffer_manager->ExecuteReadBackInstructionsReader();
	transfer_manager->ReleaseTB(readback_tbd);


	SDL_GPUCommandBuffer* cb3 = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp3 = SDL_BeginGPUCopyPass(cb3);
	TransferBufferData* postreadback_tbd = buffer_manager->ExecutePostReadbackInstructions(cp3);
	buffer_manager->ExecutePostreadBackUploadTasks(cp3, slot);
	SDL_EndGPUCopyPass(cp3);

	SDL_GPUFence* postreadbackUI_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb3);
	SDL_WaitForGPUFences(dev, true, &postreadbackUI_fence, 1);
	SDL_ReleaseGPUFence(dev, postreadbackUI_fence);
	transfer_manager->ReleaseTB(postreadback_tbd);
}

bool Engine::RenderFunc(uint8_t slot)
{
	std::lock_guard<std::mutex> scene_guard(scene_swap_mutex);

	auto t_frame = Prof::Clock::now();
	RenderCommandBuffer cb = queue_manager->GetRenderQueue().AcquireCommandBuffer();

	Uint32 w = 0, h = 0;
	SDL_GPUTexture* tex = nullptr;

	if (!cb.AcquireSwapchainTexture(win, &tex, &w, &h)) {
		cb.Cancel();
		return false;
	}
	if (!tex) {
		cb.Cancel();
		return false;
	}

	uint32_t rw, rh;
	if (size_state_.ConsumeRenderResize(rw, rh))
		texture_manager->ExecuteResizeInstructions(rw, rh);
	texture_manager->TrashTextures(slot_controller->RenderFencesDone());

	pass_manager->SetSwapchain(tex, w, h);
	pass_manager->SetRenderFrame(slot, batch_builder->AskLayout(slot));
	{
		PROF_SCOPE(Render, " execute_passes (запись команд)");
		pass_manager->ExecutePassesSteps(cb.Raw(), slot);
	}

	{
		PROF_SCOPE(Render, " imgui (new frame + UI + draw)");
		BeginImGuiFrame();
		UI_ImGui::Iterate(engine_context);
		EndImGuiFrame();
		if (imgui_draw_data && imgui_draw_data->CmdListsCount > 0)
		{
			ImGui_ImplSDLGPU3_PrepareDrawData(imgui_draw_data, cb.Raw());

			SDL_GPUColorTargetInfo imgui_color_target = {};
			imgui_color_target.texture = tex;
			imgui_color_target.load_op = SDL_GPU_LOADOP_LOAD;
			imgui_color_target.store_op = SDL_GPU_STOREOP_STORE;
			imgui_color_target.cycle = false;

			SDL_GPURenderPass* imgui_rp = cb.BeginRenderPass(&imgui_color_target, 1, nullptr);
			ImGui_ImplSDLGPU3_RenderDrawData(imgui_draw_data, cb.Raw(), imgui_rp);
			SDL_EndGPURenderPass(imgui_rp);
		}
	}

	{
		auto t = Prof::Clock::now();
		SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
		Prof::Render().Add(" submit_acquire_fence", Prof::MsSince(t));
		slot_controller->GetSlotsData()[slot].render.submit_time = Prof::Clock::now();
		slot_controller->SetRenderFence(slot, fence);
	}

	Prof::Render().Add("render_cpu (RenderFunc: запись+submit)", Prof::MsSince(t_frame));
	return true;
}

void Engine::FenceFunc(uint8_t slot) {
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];

	if (sd.render.Empty()) return;

	SDL_GPUFence* fence = sd.render.items[0];

	auto t_wait = Prof::Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	double wait_ms = Prof::MsSince(t_wait);

	double gpu_ms = Prof::MsSince(sd.render.submit_time);

	SDL_ReleaseGPUFence(dev, fence);
	sd.render.Clear();
	slot_controller->NotifyRenderFenceDone();

	slot_controller->SetSlotState(slot, SlotState::RENDERED);


	auto now = Prof::Clock::now();
	if (last_frame_done_valid) {
		double period_ms = std::chrono::duration<double, std::milli>(now - last_frame_done_time).count();
		Prof::Render().Add("frame_period (fence->fence = 1/FPS)", period_ms);
	}
	last_frame_done_time = now;
	last_frame_done_valid = true;

	Prof::Render().Add("gpu_frame (submit->fence, rabota GPU)", gpu_ms);
	Prof::Render().Add("fence_wait (CPU-blok na GPU)", wait_ms);
	PROF_FRAME(Render);
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
