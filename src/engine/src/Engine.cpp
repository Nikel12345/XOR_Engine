#include "PCH.h"
#include "Engine.h"
#include "TexturesPresets.h"
#include "ComponentSerializer.h"
#include "EngineProfiler.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

void Engine::PrepareFunc(uint8_t slot)
{
	// Слот уже зарезервирован (RESERVED) в момент выдачи — Get/WaitFreeSlotIndex
	// делает это атомарно с выбором, отдельного состояния PREPARING больше нет.
	buffer_manager->logic_index = slot;

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
		batch_builder->UpdateRenderBatches(pipe_manager, pass_manager, object_manager, object_manager->GetActiveScene());
	}

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
		slot_controller->SetSlotState(slot, UPLOADING);
		slot_controller->SetSlotState(slot, PREPARED);
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
	TransferBufferData* texture_tbd;
	{
		PROF_SCOPE(Sim, "  tex_upload_tasks");
		texture_tbd = texture_manager->ExecuteUploadTasks(cp);
	}
	SDL_EndGPUCopyPass(cp);
	{
		PROF_SCOPE(Sim, "  generate_mipmaps");
		texture_manager->GenerateMipmaps(cb);
	}
	SDL_GPUFence* fence;
	{
		PROF_SCOPE(Sim, "  submit_acquire_fence");
		fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
	}


	pending_upload_tbs[slot] = { undepended_tbd, texture_tbd };
	slot_controller->GetSlotsData()[slot].submit_time = Prof::Clock::now();
	slot_controller->SetSlotFence(slot, fence);         // fence ДО флага UPLOADING
	slot_controller->SetSlotState(slot, UPLOADING);
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
	// Реальная латентность заливки: от сабмита (PrepareFuncPrepassUndepended) до сигнала fence.
	double upload_ms = Prof::MsSince(sd.submit_time);
	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;

	// GPU дочитал transfer-буферы — возвращаем в пул (TransferManager потокобезопасен).
	auto t_rel = Prof::Clock::now();
	transfer_manager->ReleaseTB(pending_upload_tbs[slot].buffers_tbd);
	transfer_manager->ReleaseTB(pending_upload_tbs[slot].textures_tbd);
	pending_upload_tbs[slot] = {};
	double release_ms = Prof::MsSince(t_rel);

	slot_controller->SetSlotState(slot, PREPARED);

	// upload_gpu — сколько реально длится заливка на GPU (submit -> fence). fence_wait —
	// сколько CPU-поток блокировался на ней. Если upload_gpu велик и слотов не хватает,
	// sim увидит это как рост slot_wait в SIM-отчёте.
	Prof::Upload().Add("upload_gpu (submit->fence, заливка на GPU)", upload_ms);
	Prof::Upload().Add("upload_fence_wait (CPU-блок)", wait_ms);
	Prof::Upload().Add("release_tbs (возврат TB в пул)", release_ms);
	PROF_FRAME(Upload);
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

	if (texture_manager->main_pass_depth)
		texture_manager->main_pass_depth->Resize(w, h);
	DefaultRenderPassNamespace::ResizeSceneHDRTargets(engine_context, w, h);

	if (RenderPassStep* present_rp = pass_manager->GetRenderPassStep(DefaultRenderPassNamespace::PRESENT_PASS))
		present_rp->renderPassTexsData.SetColorTexture(tex);
	pass_manager->SetRenderFrame(slot);
	{
		PROF_SCOPE(Render, " execute_passes (запись команд)");
		std::lock_guard<std::mutex> batch_lock(pass_manager->BatchTreeMutex());
		pass_manager->ExecutePassesSteps(cb, slot);
	}

	{
		PROF_SCOPE(Render, " imgui (new frame + UI + draw)");
		BeginImGuiFrame();
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
		// Метку сабмита пишем ДО публикации fence — FenceFunc замерит от неё
		// GPU-латентность кадра (submit -> сигнал fence). Поле общее с upload (см. SlotData).
		slot_controller->GetSlotsData()[slot].submit_time = Prof::Clock::now();
		// IS_RENDERING на слоте уже стоит — его поставил WaitRenderableSlot в момент
		// выбора (атомарно, чтобы sim не захватил слот до сабмита). Здесь только fence.
		slot_controller->SetSlotFence(slot, fence);
	}

	// ВНИМАНИЕ: это только CPU-время ЗАПИСИ и сабмита командного буфера (доли мс).
	// Оно НЕ равно времени кадра/FPS: сам кадр исполняется на GPU асинхронно, а его
	// завершение и РЕАЛЬНЫЙ период кадра замеряет FenceFunc (там же и Frame()).
	Prof::Render().Add("render_cpu (RenderFunc: запись+submit)", Prof::MsSince(t_frame));
	return true;
}

void Engine::FenceFunc(uint8_t slot) {
	SlotData* slots = slot_controller->GetSlotsData();
	SlotData& sd = slots[slot];
	SDL_GPUFence* fence = sd.fence;

	if (!fence) return;

	// Блокирующее ожидание вместо опроса: неблокирующий SDL_QueryGPUFence в цикле
	// FenceThread был busy-poll'ом — ядро выгорало на всё время исполнения кадра GPU,
	// и CPU-нагрузка росла вместе с GPU-нагрузкой. Kernel-wait спит до сигнала fence.
	auto t_wait = Prof::Clock::now();
	SDL_WaitForGPUFences(dev, true, &fence, 1);
	double wait_ms = Prof::MsSince(t_wait);
	// Реальная работа GPU над кадром: от сабмита (RenderFunc) до сигнала fence (сейчас).
	// В отличие от fence_wait это не зависит от того, когда CPU добрался до ожидания.
	double gpu_ms = Prof::MsSince(sd.submit_time);

	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;
	buffer_manager->TrashBuffers();
	texture_manager->TrashTextures();   // отложенное удаление текстур — та же кадровая точка, что и буферы

	//slot_controller->RemoveSlotFence(slot);
	slot_controller->SetSlotState(slot, RENDERED);

	// ── ПРОФАЙЛ завершения кадра. Это НАСТОЯЩАЯ кадровая точка (RenderFunc лишь
	//    пишет команды и мгновенно возвращается). frame_period — интервал между
	//    завершениями соседних кадров: его среднее = 1/FPS. Сравнение:
	//      frame_period ≈ gpu_frame  → упор в GPU;
	//      frame_period ≫ gpu_frame  → GPU простаивает (упор в sim/синхронизацию/слипы).
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

void Engine::OnWindowResized(Sint32 w, Sint32 h)
{
	width = safe_sint32_f(w);
	height = safe_sint32_f(h);

	// GPU-таргеты (depth + HDR) ресайзятся НЕ здесь, а в RenderFunc по размеру реально полученного
	// свопчейна — раз на отрисованный кадр. Событие resized летит сотнями за drag; пересоздавать
	// большие текстуры на каждое = VRAM-захлёб и DEVICE_LOST. Здесь только обновляем width/height
	// (для aspect-ratio камеры и пр.).
}

Engine::Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height)
{
	this->win = window;
	this->dev = dev;
	this->width = width;
	this->height = height;
	transfer_manager = new TransferManager(dev);
	buffer_manager = new BufferManager(dev, transfer_manager);
	texture_manager = new TextureManager(dev, transfer_manager);
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
	instance_data_module = new InstanceDataModule();
	light_data_module = new LightDataModule();
	indirect_data_module = new IndirectDataModule();
	bound_sphere_data_module = new BoundSphereDataModule();
	count_data_module = new CountBufferDataModule();

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	InitDefaultBufferUpdaters();
	InitPasses();
	InitUICommands();
	RegisterBuiltinComponentSerializers();   // сериалайзеры компонентов для save/load сцены
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

	engine_context->CreateTextureAtlas("_FallbackAtlas", TexturePresets::AlbedoAtlas(64, 1, 1), "_SimpleSampler");
	TextureHandle* dummy = engine_context->CreateTextureFromFile("_NoTextureDummy", "_FallbackAtlas", "../engine/textures/dummy.png");

	batch_builder->SetDummyTexture(dummy);
}

void Engine::InitDefaultBufferUpdaters()
{
	using namespace DefaultUpdateSet;

	SetDefaultCameraUpdater(*engine_context);
	SetDefaultPositionUpdater(*engine_context, transform_data_module);
	SetDefaultInstanceDataUpdater(*engine_context, instance_data_module);
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

	{
		_SetDefaultCommonResources(engine_context, safe_f_u32(width), safe_f_u32(height));
		SetDefaultShadowPCFRenderPass(engine_context);
		SetDefaultMainRenderPass(engine_context);
		SetTransparentPass(engine_context);
		SetDebugColliderPass(engine_context);
		SetDefaultBloomPass(engine_context);       // bloom от эмиссии (compute) + composite/tonemap в scene_hdr
		SetPresentPass(engine_context);            // финал: HDR-сцену в свопчейн (blit)
	}
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

	input_manager->RegisterCommand(CommandId::HideEntity,
		[](EngineContext* ctx, const void* data)
		{
			// Данных-структуры нет — Entity и флаг упакованы прямо в указатель:
			// младшие 32 бита — Entity, бит 32 — visible (см. UI_ImGui::DrawObjectsPanel).
			const uintptr_t packed = reinterpret_cast<uintptr_t>(data);
			const Entity e = static_cast<Entity>(packed & 0xFFFFFFFFu);
			const bool visible = ((packed >> 32) & 0x1u) != 0u;
			ctx->HideEntity(ctx->GetObjectManager()->GetActiveSceneName(), e, visible);
		});

	// Правка трансформа гизмой. Продьюсер (UI) выделил SetTransformCmd на куче — здесь
	// пишем матрицу в Positions выбранной сущности и освобождаем payload. Источник —
	// мировая матрица (column-major glm от ImGuizmo); Positions хранит её row-major,
	// поэтому раскладываем поэлементно: трансляция уходит в w/d/h (как в остальном UI).
	input_manager->RegisterCommand(CommandId::SetTransform,
		[](EngineContext* ctx, const void* data)
		{
			const SetTransformCmd* c = static_cast<const SetTransformCmd*>(data);
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetActiveScene();
			// Сущность могла быть удалена между push и исполнением — Has это отсекает.
			if (scene && om->Has<Positions>(scene, c->entity))
			{
				SoAElement<Positions> el = om->GetComponent<Positions>(scene, c->entity);
				Positions& P = el.container();
				const size_t i = el.i();
				const float* m = c->matrix;   // column-major: m[col*4 + row]
				P.x[i] = m[0]; P.y[i] = m[4]; P.z[i] = m[8];  P.w[i] = m[12];
				P.a[i] = m[1]; P.b[i] = m[5]; P.c[i] = m[9];  P.d[i] = m[13];
				P.e[i] = m[2]; P.f[i] = m[6]; P.g[i] = m[10]; P.h[i] = m[14];
				P.i[i] = m[3]; P.j[i] = m[7]; P.k[i] = m[11]; P.l[i] = m[15];
			}
			delete c;
		});

	// Save/Load сцены — в sim-потоке (мутация ECS + взвод пересборки батчей). Payload
	// (имя+путь) выделен на куче в UI, удаляем после применения.
	input_manager->RegisterCommand(CommandId::SaveScene,
		[](EngineContext* ctx, const void* data)
		{
			const SceneIOCmd* c = static_cast<const SceneIOCmd*>(data);
			ctx->SaveScene(c->scene, c->path);
			delete c;
		});

	input_manager->RegisterCommand(CommandId::LoadScene,
		[](EngineContext* ctx, const void* data)
		{
			const SceneIOCmd* c = static_cast<const SceneIOCmd*>(data);
			ctx->LoadScene(c->scene, c->path);
			delete c;
		});
}


Engine::~Engine()
{
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();

	delete buffer_manager;
	delete texture_manager;
	delete transfer_manager;   // после менеджеров: они возвращают арендованные TB в пул
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
