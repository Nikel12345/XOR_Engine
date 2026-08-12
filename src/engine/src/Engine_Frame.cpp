#include "PCH.h"
#include "Engine.h"
#include "EngineProfiler.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "BufferManager.h"
#include "TextureManager.h"
#include "PipeManager.h"
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

//  Кадровый конвейер Engine: prepare → upload → render → fence.
//  Колбэки ThreadController (см. Engine::Engine), каждый на своём потоке.

void Engine::PrepareFunc(uint8_t slot)
{
	// Слот уже зарезервирован (RESERVED) в момент выдачи — Get/WaitFreeSlotIndex
	// делает это атомарно с выбором, отдельного состояния PREPARING больше нет.
	buffer_manager->logic_index = slot;

	// UI-раскладка (Yoga): считает rect'ы и создаёт/пересоздаёт UI-энтити ДО сборки батчей, чтобы
	// новые/изменённые элементы попали в этот же кадр. No-op, если не грязно (грязь ставят
	// построение дерева игрой и ресайз окна). Размер кадра — из size_state_.render_size (GetWidth/Height).
	ui_yoga->Emit(engine_context, GetWidth(), GetHeight());

	// Aspect камеры = соотношение render-разрешения (size_state_.render_size). Считалось 1 раз в ctor
	// Camera и на ресайз не обновлялось → 3D растянут при смене render_size. Engine оркеструет (владеет
	// и camera_manager, и размером), sim-поток монопольно мутирует камеру. GetProj() читает aspect вживую.
	if (Camera* cam = camera_manager->GetActiveCamera()) {
		const float h = GetHeight();
		if (h > 0.0f) cam->SetAspect(GetWidth() / h);
	}

	// Отложенные удаления GPU-ресурсов: каждый трэш дренирует ПОТОК-ВЛАДЕЛЕЦ, локов нет.
	// Буферы/пайплайны — sim (пуш и дренаж здесь); текстуры — render (пуш и дренаж в RenderFunc:
	// таргеты пересоздаются под свопчейн там же). FenceThread лишь тикает счётчик render-fence.
	// Запись освобождается после BUFFERING_LEVEL render-fence с её стампа (см. Trash*);
	// пайплайны — ещё и после армирования эпохой (см. PipeManager::TrashPipelines).
	{
		const uint64_t fences_done = slot_controller->RenderFencesDone();
		buffer_manager->TrashBuffers(fences_done);
		pipe_manager->TrashPipelines(fences_done, slot_controller->RequiredEpoch(), batch_builder->ComputeRebuildEpoch());
	}

	// ── БЕЙК GPU-РЕСУРСОВ ──
	// Create* только ОБЪЯВЛЯЮТ обёртки (BufferData/TextureAtlas); usage-флаги им наполняют
	// ДЕКЛАРАЦИИ (sp/материалы/проходы/блиты), и сами SDL_GPUBuffer/SDL_GPUTexture рождаются
	// здесь — уже по собранным флагам. Место выбрано так, что:
	//   — игровой апдейт (game_iter_callback: создание ресурсов, ExecuteCommands, LoadScene) идёт
	//     РАНЬШЕ prepare на этом же sim-потоке → всё, объявленное в кадре N, создаётся в кадре N;
	//   — все декларации, дающие ресурсу флаги, к этому моменту сделаны;
	//   — а PackAtlases и сборка батчей НИЖЕ уже требуют готовые GPU-хэндлы (они копируют
	//     texture_binding по значению в upload-таски и в слепок), поэтому бейк обязан быть до них.
	// Дренаж пер-кадровый, а не разовый: игра может завести новый буфер/атлас в любой момент.
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

	// Клеймим слоту текущую эпоху ребилда (до заливки его буферов ниже). Если этот prepare
	// был полным ребилдом — планка в SlotController поднимется, и рендер не покажет слоты со
	// старой раскладкой indirect/out_pib (короткий hold вместо мерцания). Иначе — инертно.
	slot_controller->StampSlotEpoch(slot, batch_builder->RebuildEpoch());

	// Слепок раскладки батчей слоту (O(1), shared_ptr): всё, что prepare зальёт в буферы
	// слота ниже, соответствует именно этой версии раскладки — рендер рисует по ней же
	// (AskLayout/AskNum*(slot)), живое дерево ему не нужно.
	batch_builder->StampLayoutSnapshot(slot);

	{
		PROF_SCOPE(Sim, " build_compute_batches");
		batch_builder->BuildComputeBatches(pass_manager, pipe_manager, shader_manager);
	}

	// Prepass отправляет загрузку АСИНХРОННО: слот уйдёт в UPLOADING, а PREPARED его
	// сделает UploadThread по сигналу upload-fence. Sim здесь больше не ждёт GPU.
	{
		PROF_SCOPE(Sim, " prepass_undepended (submit загрузки)");
		PrepareFuncPrepassUndepended(slot);
	}
	// A/B-прогон префасса (стенд в Engine_PrepassBench.cpp, под ENGINE_BENCH). Оставлено
	// как есть для восстановления: g_stats_*/_Original/_Optimized сейчас static в bench-TU,
	// поэтому для реального включения их надо вызвать через один экспорт из стенда.
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
		SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
		TransferBufferData* tbd;
		{
			PROF_SCOPE(Sim, "  exec_update_instructions (всего)");
			tbd = buffer_manager->ExecuteUpdateInstructions(cp);
		}
		SDL_EndGPUCopyPass(cp);
		SDL_CancelGPUCommandBuffer(cb);
		transfer_manager->ReleaseTB(tbd);
		slot_controller->SetSlotState(slot, SlotState::UPLOADING);
		slot_controller->SetSlotState(slot, SlotState::PREPARED);
		return;
	}

	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);

	TransferBufferData* undepended_tbd;
	{
		PROF_SCOPE(Sim, "  exec_update_instructions (всего)");
		undepended_tbd = buffer_manager->ExecuteUpdateInstructions(cp);
	}
	{
		PROF_SCOPE(Sim, "  exec_upload_tasks (буферы)");
		buffer_manager->ExecuteUploadTasks(cp, slot);
	}
	SDL_EndGPUCopyPass(cp);
	// Текстур здесь больше НЕТ: заливка пикселей, мипы и блиты превью уехали в RenderFunc.
	// Причина не в производительности — мипы (SDL_GenerateMipmapsForGPUTexture) и превью
	// (SDL_BlitGPUTexture) это ОТРИСОВКА, и на копировальной очереди их не исполнить. Пока они
	// сидели в этом cb, весь upload был непереносим на отдельную очередь. Sim теперь только
	// упаковывает атласы и передаёт пачку (TextureManager::PackAtlases → _PublishUploadTasks).
	SDL_GPUFence* fence;
	{
		PROF_SCOPE(Sim, "  submit_acquire_fence");
		fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
	}


	pending_upload_tbs[slot] = undepended_tbd;
	slot_controller->GetSlotsData()[slot].submit_time = Prof::Clock::now();
	slot_controller->SetSlotFence(slot, fence);         // fence ДО флага UPLOADING
	slot_controller->SetSlotState(slot, SlotState::UPLOADING);
}

// Зеркало FenceFunc для upload-fence. Блокирующее ожидание здесь обязательно:
// порядок сабмитов и барьеры copy-пасса на практике НЕ гарантируют, что данные
// дошли до GPU к моменту чтения кадром — единственный надёжный сигнал это fence.
void Engine::UploadFunc(uint8_t slot)
{
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];
	SDL_GPUFence* fence = sd.fence;

	if (!fence) return;

	auto t_wait = Prof::Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	double wait_ms = Prof::MsSince(t_wait);
	double upload_ms = Prof::MsSince(sd.submit_time);
	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;

	auto t_rel = Prof::Clock::now();
	transfer_manager->ReleaseTB(pending_upload_tbs[slot]);
	pending_upload_tbs[slot] = nullptr;
	double release_ms = Prof::MsSince(t_rel);

	slot_controller->SetSlotState(slot, SlotState::PREPARED);

	Prof::Upload().Add("upload_gpu (submit->fence, заливка на GPU)", upload_ms);
	Prof::Upload().Add("upload_fence_wait (CPU-блок)", wait_ms);
	Prof::Upload().Add("release_tbs (возврат TB в пул)", release_ms);
	PROF_FRAME(Upload);
}

// Вычислительная стадия конвейера — между загрузкой и рендером, на своём потоке и своём слоте.
//
// СЕЙЧАС ПУСТАЯ, и это не заглушка-обход: слот честно проходит стадию, просто работы в ней нет.
// Конвейер полный, и когда сюда переедет GPU-каллинг (сейчас он живёт отдельным cb + fence
// внутри RenderFunc), менять придётся только тело — ни слотовая машина, ни потоки не тронутся.
//
// Смысл будущего переезда: каллинг перестанет ждать, пока render-поток дорисует текущий кадр.
// Он возьмёт СВОЙ слот и посчитает его на вычислительной очереди, пока графическая занята
// предыдущим — то есть перекрытие появится между кадрами, а не внутри одного.
//
// Слот сюда приходит уже помеченным IS_COMPUTING (его ставит WaitComputableSlot при захвате,
// иначе окно между выбором и сабмитом было бы гонкой). Переход в COMPUTING идемпотентен и
// оставлен явно: стадия объявляет своё состояние так же, как остальные.
void Engine::ComputeFunc(uint8_t slot)
{
	slot_controller->SetSlotState(slot, SlotState::COMPUTING);

	// ← здесь появится ExecutePrepassesSteps на вычислительной очереди + свой fence

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
	// Кадр целиком под замком свопа сцены (см. scene_swap_mutex): пока LoadScene сносит и
	// пересоздаёт ECS/дерево UI/словари ресурсов, рендер-поток просто стоит. Берём ДО замера
	// времени — ожидание загрузки это не кадр, и в render_cpu ему делать нечего.
	std::lock_guard<std::mutex> scene_guard(scene_swap_mutex);

	auto t_frame = Prof::Clock::now();
	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);

	Uint32 w = 0, h = 0;
	SDL_GPUTexture* tex = nullptr;

	// Ранний выход (нет свопчейн-текстуры) кадром не считаем — Frame() не зовём.
	if (!SDL_AcquireGPUSwapchainTexture(cb, win, &tex, &w, &h)) {
		SDL_CancelGPUCommandBuffer(cb);
		return false;
	}
	if (!tex) {
		SDL_CancelGPUCommandBuffer(cb);
		return false;
	}

	// Ресайз экранных таргетов — ТОЛЬКО здесь, на render-потоке (владельце трэш-очереди). Гейт целиком
	// в структуре (ConsumeRenderResize сравнивает желаемое render_size со своим прошлым): пересоздаём
	// таргеты лишь когда меняется ВНУТРЕННЕЕ разрешение (SetRenderResolution из sim — «полноценный
	// ресайз» по кнопке игрового UI). Смена ОКНА (window_size, main) таргеты НЕ трогает — там только
	// свопчейн + растяжение present-блитом. Логика per-таргет — в инструкциях (_SetDefaultCommonResources).
	uint32_t rw, rh;
	if (size_state_.ConsumeRenderResize(rw, rh))
		texture_manager->ExecuteResizeInstructions(rw, rh);
	// Текстурный трэш дренирует его владелец — render-поток (старые таргеты кладутся строками
	// выше при ресайзе под свопчейн); буферы/пайплайны так же монопольно дренирует sim в PrepareFunc.
	texture_manager->TrashTextures(slot_controller->RenderFencesDone());

	// ── ТЕКСТУРНАЯ РАБОТА ──
	// Живёт здесь, а не в prepare, потому что мипы и превью — ОТРИСОВКА (SDL_GenerateMipmaps* /
	// SDL_BlitGPUTexture), и очередь, умеющая только копировать, их не исполнит. Держать их в
	// upload-cb значило запереть заливку на графической очереди навсегда.
	// Место в кадре: ПОСЛЕ проверок свопчейна (раньше — cb мог быть отменён, и забранная пачка
	// пропала бы вместе с ним) и ПОСЛЕ ресайза таргетов, но ДО любых проходов, которые атласы
	// читают. Порядок внутри cb гарантирует, что к отрисовке пиксели и мипы уже на месте.
	TransferBufferData* texture_tbd;
	{
		PROF_SCOPE(Render, " tex_upload (заливка+мипы+превью)");
		SDL_GPUCopyPass* tex_cp = SDL_BeginGPUCopyPass(cb);
		texture_tbd = texture_manager->ExecuteUploadTasks(tex_cp);
		SDL_EndGPUCopyPass(tex_cp);
		texture_manager->GenerateMipmaps(cb);
		texture_manager->BlitPendingPreviews(cb);   // ПОСЛЕ пикселей и мипов — источник готов
	}

	// Свопчейн — в свой атлас (PassManager владеет им как полем, вне реестра TextureManager).
	// Отсюда его берёт PRESENT_PASS (блит-проход). Пишем только мы — RenderFunc единственный
	// писатель, он же и читатель (тело блита в ExecutePassesSteps ниже) → без замков.
	pass_manager->SetSwapchain(tex, w, h);
	// Слот + его слепок раскладки: scatter и draw ниже читают ТОЛЬКО пер-слотовые слепки
	// (BatchLayout, таблица теневых камер) — живые ECS/дерево батчей рендеру не нужны,
	// замков нет, sim свободно мутирует своё параллельно.
	pass_manager->SetRenderFrame(slot, batch_builder->AskLayout(slot));
	{
		PROF_SCOPE(Render, " execute_passes (запись команд)");

		// GPU-каллинг (scatter) в ОТДЕЛЬНОМ cb + fence ПЕРЕД рендером. SDL_GPU не барьерит
		// compute-write -> indirect-read внутри одного cb (на 1М scatter из-за atomic-контеншена
		// медленный, и draw читал недозаполненный индирект → мерцание). Отдельный cb + fence
		// гарантирует, что out_pib/indirect дописаны. Fence вдобавок ТРОТТЛИТ рендер-поток под
		// темп GPU: без него (просто сабмит) рендер убегает вперёд, копит бэклог GPU и пейсинг
		// ХУЖЕ (50мс-спайки). Консистентность scatter↔draw даёт общий слепок слота.
		{
			SDL_GPUCommandBuffer* cull_cb = SDL_AcquireGPUCommandBuffer(dev);
			pass_manager->ExecutePrepassesSteps(cull_cb, slot);
			SDL_GPUFence* cull_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cull_cb);
			SDL_WaitForGPUFences(dev, true, &cull_fence, 1);
			SDL_ReleaseGPUFence(dev, cull_fence);
		}

		pass_manager->ExecutePassesSteps(cb, slot);
	}

	{
		PROF_SCOPE(Render, " imgui (new frame + UI + draw)");
		BeginImGuiFrame();
		// UI читает живую сцену (иерархия/инспектор/гизмо) с рендер-потока БЕЗ замка —
		// осознанный компромисс (на тестах стабильно; мутации UI гонит через очередь
		// команд в sim). Правильное закрытие — UI-слепок или построение UI в sim-потоке.
		UI_ImGui::Iterate(engine_context);
		EndImGuiFrame();
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
	}

	{
		auto t = Prof::Clock::now();
		SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
		Prof::Render().Add(" submit_acquire_fence", Prof::MsSince(t));
		slot_controller->GetSlotsData()[slot].submit_time = Prof::Clock::now();
		// Текстурный TB читается GPU в этом же cb → отпускать только по ЭТОМУ fence (FenceFunc).
		// Стеш пишем ДО публикации fence — та же схема видимости, что у pending_upload_tbs.
		pending_render_tbs[slot] = texture_tbd;
		slot_controller->SetSlotFence(slot, fence);
	}

	Prof::Render().Add("render_cpu (RenderFunc: запись+submit)", Prof::MsSince(t_frame));
	return true;
}

void Engine::FenceFunc(uint8_t slot) {
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];
	SDL_GPUFence* fence = sd.fence;

	if (!fence) return;

	auto t_wait = Prof::Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	double wait_ms = Prof::MsSince(t_wait);

	double gpu_ms = Prof::MsSince(sd.submit_time);

	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;
	// Текстурный TB: GPU дочитал его вместе с кадром — вернуть в пул (см. pending_render_tbs).
	transfer_manager->ReleaseTB(pending_render_tbs[slot]);
	pending_render_tbs[slot] = nullptr;
	// Жизнь GPU-ресурсов FenceThread не трогает — только тикает счётчик завершённых render-fence.
	// Дренаж трэшей (buffers/textures/pipelines) делает sim в PrepareFunc (очереди одно-поточные).
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
