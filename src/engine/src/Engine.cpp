#include "PCH.h"
#include "Engine.h"
#include "TexturesPresets.h"
#include "ComponentSerializer.h"
#include "MaterialParams.h"           // Opaque/TransparentMaterialParams — билтин-типы params
#include "MaterialParamsRegistry.h"   // реестр типов params (дропдаун Kind)
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
		batch_builder->UpdateRenderBatches(pipe_manager, pass_manager, object_manager, texture_manager, shader_manager, object_manager->GetActiveScene());
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
	double upload_ms = Prof::MsSince(sd.submit_time);
	SDL_ReleaseGPUFence(dev, fence);
	sd.fence = nullptr;

	auto t_rel = Prof::Clock::now();
	transfer_manager->ReleaseTB(pending_upload_tbs[slot].buffers_tbd);
	transfer_manager->ReleaseTB(pending_upload_tbs[slot].textures_tbd);
	pending_upload_tbs[slot] = {};
	double release_ms = Prof::MsSince(t_rel);

	slot_controller->SetSlotState(slot, PREPARED);

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

		// GPU-каллинг (scatter) в ОТДЕЛЬНОМ cb + fence ПЕРЕД рендером. SDL_GPU не барьерит
		// compute-write -> indirect-read внутри одного cb (на 1М scatter из-за atomic-контеншена
		// медленный, и draw читал недозаполненный индирект → мерцание). Отдельный cb + fence
		// гарантирует, что out_pib/indirect дописаны. Fence вдобавок ТРОТТЛИТ рендер-поток под
		// темп GPU: без него (просто сабмит) рендер убегает вперёд, копит бэклог GPU и пейсинг
		// ХУЖЕ (50мс-спайки). Под тем же batch-замком — дерево консистентно для scatter и draw.
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
		slot_controller->SetSlotFence(slot, fence);
	}

	Prof::Render().Add("render_cpu (RenderFunc: запись+submit)", Prof::MsSince(t_frame));
	return true;
}

// [DEBUG] Проверка GPU-каллинга: раз в ~60 кадров скачиваем первые 10 значений out_pib
// слота (блок 0 = камера игрока) и печатаем. Зовётся из FenceFunc ПОСЛЕ render-fence
// (compute уже записал буфер) и ДО перевода слота в RENDERED (слот не переиспользуется,
// пока идёт синхронный download). Временный код — удалить после проверки.
// [DEBUG] Скачивает регион GPU-буфера в CPU-вектор (синхронно, свой cb+fence). Только дебаг.
static std::vector<uint32_t> DbgDownload(SDL_GPUDevice* dev, SDL_GPUBuffer* buf, uint32_t byte_off, uint32_t byte_len)
{
	std::vector<uint32_t> out(byte_len / 4);
	SDL_GPUTransferBufferCreateInfo ci{};
	ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	ci.size = byte_len;
	SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ci);
	if (!tb) return out;
	SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
	SDL_GPUBufferRegion src{ buf, byte_off, byte_len };
	SDL_GPUTransferBufferLocation dst{ tb, 0 };
	SDL_DownloadFromGPUBuffer(cp, &src, &dst);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
	SDL_WaitForGPUFences(dev, true, &f, 1);
	SDL_ReleaseGPUFence(dev, f);
	const void* p = SDL_MapGPUTransferBuffer(dev, tb, false);
	if (p) { memcpy(out.data(), p, byte_len); SDL_UnmapGPUTransferBuffer(dev, tb); }
	SDL_ReleaseGPUTransferBuffer(dev, tb);
	return out;
}

// [DEBUG] ПРОВЕРКА КОМПАКТАЦИИ: на первой РИСУЕМОЙ команде блока 0 читаем её диапазон out_pib
// [first_instance, first_instance+num_instances) и ищем -1 и ДУБЛИКАТЫ строк. Их наличие = out_pib
// битый (баг scatter/компактации). Чисто = out_pib корректен → артефакт в другом (трансформ/слот).
// Читается пост-кадрово (в FenceFunc, fence слота прошёл) → показывает то, что scatter реально записал.
void Engine::DebugReadbackOutPib(uint8_t slot)
{
	static int frame_counter = 0;
	if (++frame_counter % 20 != 0) return;

	uint32_t tc = batch_builder->AskNumCommands();
	uint32_t N  = batch_builder->AskNumInstances();
	if (tc == 0 || N == 0) return;
	uint32_t num_cameras = 1 + light_data_module->AskNumLightCameras(object_manager, object_manager->GetActiveScene());

	BufferData* ind_bd = buffer_manager->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER);
	BufferData* pib_bd = buffer_manager->GetBufferData(DefaultBuffersNames::DEFAULT_OUT_PIB_BUFFER);
	if (!ind_bd || !pib_bd) return;
	SDL_GPUBuffer* ind_buf = buffer_manager->_GetGPUBufferForFrame(ind_bd, slot);
	SDL_GPUBuffer* pib_buf = buffer_manager->_GetGPUBufferForFrame(pib_bd, slot);
	if (!ind_buf || !pib_buf) return;

	// Проверяем блок 0 (игрок) и блок 1 (первая световая камера, если есть) — обе scatter-программы.
	uint32_t blocks_to_check = num_cameras < 2u ? num_cameras : 2u;
	for (uint32_t c = 0; c < blocks_to_check; ++c) {
		// Индирект блока c: tc команд по 5 uint (num_indices, num_instances, first_index, vertex_offset, first_instance).
		std::vector<uint32_t> ind = DbgDownload(dev, ind_buf, c * tc * 20u, tc * 20u);
		uint32_t cmdK = 0xFFFFFFFFu, firstInst = 0, numInst = 0;   // КРУПНЕЙШАЯ команда блока
		for (uint32_t k = 0; k < tc; ++k) {
			uint32_t num_indices   = ind[k * 5 + 0];
			uint32_t num_instances = ind[k * 5 + 1];
			if (num_indices > 0 && num_instances > numInst) { cmdK = k; numInst = num_instances; firstInst = ind[k * 5 + 4]; }
		}
		if (cmdK == 0xFFFFFFFFu) { SDL_Log("[VERIFY out_pib] slot=%u block=%u — пусто", slot, c); continue; }

		uint32_t cap = numInst < 300000u ? numInst : 300000u;
		std::vector<uint32_t> pib = DbgDownload(dev, pib_buf, (c * N + firstInst) * 4u, cap * 4u);

		int neg1 = 0, oob = 0, dups = 0;
		std::unordered_set<int32_t> seen;
		seen.reserve(cap * 2);
		for (uint32_t i = 0; i < cap; ++i) {
			int32_t r = static_cast<int32_t>(pib[i]);
			if (r < 0) { neg1++; continue; }
			if ((uint32_t)r >= N) { oob++; continue; }
			if (!seen.insert(r).second) dups++;
		}
		SDL_Log("[VERIFY out_pib] slot=%u block=%u cmd=%u num=%u checked=%u | neg1=%d oob=%d dups=%d %s",
			slot, c, cmdK, numInst, cap, neg1, oob, dups, (neg1 || oob || dups) ? "<<< БИТО" : "ok");
	}
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
	buffer_manager->TrashBuffers();
	texture_manager->TrashTextures();

	//DebugReadbackOutPib(slot);   // [DEBUG] выключен; проверка out_pib на -1/oob/дубли (см. функцию)

	//slot_controller->RemoveSlotFence(slot);
	slot_controller->SetSlotState(slot, RENDERED);


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

	engine_context = new EngineContext(buffer_manager, texture_manager, pass_manager, material_manager, object_manager, shader_manager, model_manager, camera_manager, pipe_manager, batch_builder, texture_loader);
	engine_context->SetInputManager(input_manager);
	InitDefaultBufferUpdaters();
	InitPasses();
	InitUICommands();
	RegisterBuiltinComponentSerializers();   // сериалайзеры компонентов для save/load сцены

	// Билтин-типы params для UI-дропдауна Kind. Пользователь дорегистрирует свои из кода игры
	// (MaterialParamsRegistry::Get().Register{...}) — движок трогать не надо.
	{
		auto& r = MaterialParamsRegistry::Get();
		// None: applyDefault чистит блоб; edit пустой (нечего рисовать).
		r.Register({ MaterialParamsKind::None, "None",
			[](EngineContext*, Material* m){ m->params.clear(); m->params_kind = MaterialParamsKind::None; } });
		r.Register({ MaterialParamsKind::Opaque, "Opaque",
			[](EngineContext* ctx, Material* m){ ctx->SetMaterialParams(m, OpaqueMaterialParams{}); },
			[](Material* m){
				if (m->params.size() < sizeof(OpaqueMaterialParams)) return;   // защита от рассинхрона kind/блоба
				auto* p = reinterpret_cast<OpaqueMaterialParams*>(m->params.data());
				ImGui::ColorEdit3("Base Color", p->baseColor);
				ImGui::ColorEdit3("Emissive", p->emissive);
				ImGui::SliderFloat("Emissive Strength", &p->emissiveStrength, 0.0f, 8.0f);
				ImGui::SliderFloat("Metallic", &p->metallic, 0.0f, 1.0f);
				ImGui::SliderFloat("Roughness", &p->roughness, 0.0f, 1.0f);
			} });
		r.Register({ MaterialParamsKind::Transparent, "Transparent",
			[](EngineContext* ctx, Material* m){ ctx->SetMaterialParams(m, TransparentMaterialParams{}); },
			[](Material* m){
				if (m->params.size() < sizeof(TransparentMaterialParams)) return;
				auto* p = reinterpret_cast<TransparentMaterialParams*>(m->params.data());
				ImGui::SliderFloat("Alpha", &p->alpha, 0.0f, 1.0f);
			} });
	}
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
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // docking-ветка ImGui: окна можно стыковать (см. будущий DockSpace)

	ImGui_ImplSDL3_InitForSDLGPU(window);

	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = dev;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&init_info);

	engine_context->CreateTextureAtlas("_FallbackAtlas", TexturePresets::AlbedoAtlas(64, 1, 1), "_SimpleSampler");
	TextureHandle* dummy = engine_context->CreateTextureFromFile("_NoTextureDummy", "_FallbackAtlas", "../engine/textures/dummy.png");

	batch_builder->SetDummyTexture(dummy);
	InitDefaultResources();
}

void Engine::InitDefaultResources()
{
	// Все material-атласы — BGRA8 UNORM (движок без sRGB-форматов), поэтому дефолты кладём в один
	// движковый _FallbackAtlas: цвет читается как есть, нормаль тоже (без sRGB-декода). Пиксели —
	// BGRA (все каналы равны → порядок неважен). Материалы ссылаются по имени, атлас безразличен.
	texture_manager->CreateTexture("default_albedo",   "_FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0xFF }));   // белый
	texture_manager->CreateTexture("default_normal",   "_FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0x80 }));   // 128,128,128,128 (высота в альфе)
	texture_manager->CreateTexture("default_orm",      "_FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));
	texture_manager->CreateTexture("default_emissive", "_FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF }));
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
	SetDefaultIndirectUpdater(*engine_context, indirect_data_module, light_data_module);

	// GPU-каллинг с компактацией: сферы по строкам + entity->cmd (ревизия батчей) +
	// ресайз out_pib (компактно пишет scatter-каллинг). Индирект — per-frame выше.
	SetDefaultBoundSphereUpdater(*engine_context, bound_sphere_data_module);
	SetDefaultEntityToCmdUpdater(*engine_context, pib_data_module);
	SetDefaultOutPibUpdater(*engine_context, light_data_module);
}

void Engine::InitPasses()
{
	using namespace DefaultRenderPassNamespace;

	{
		_SetDefaultCommonResources(engine_context, safe_f_u32(width), safe_f_u32(height));
		SetDefaultCullingPass(engine_context);     // GPU-каллинг: out_pib до SHADOW_PASS (индекс 5)
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
}

// Дефолтная текстура по роли слота (движковые из InitDefaultResources). Custom* → dummy.
static const char* DefaultTextureForRole(TextureSlotRole r)
{
	switch (r) {
	case TextureSlotRole::Albedo:   return "default_albedo";
	case TextureSlotRole::Normal:   return "default_normal";
	case TextureSlotRole::ORM:      return "default_orm";
	case TextureSlotRole::Emissive: return "default_emissive";
	default:                        return "_NoTextureDummy";   // Custom* и прочее
	}
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

	// Смена текстуры слота материала — в sim-потоке: правим имя в Material::textures[role]
	// (name-based ссылка) и взводим ПЕРЕСБОРКУ батчей (в батче запечён разрешённый UVL текстуры).
	input_manager->RegisterCommand(CommandId::SetMaterialTexture,
		[](EngineContext* ctx, const void* data)
		{
			const SetMaterialTextureCmd* c = static_cast<const SetMaterialTextureCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material))
				m->textures[static_cast<TextureSlotRole>(c->role)] = c->texture;
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Upsert текстуры — в sim-потоке: delete-if-exists + загрузка из файла (edit=create, без ветвлений).
	// Ребилд батчей: материалы, ссылающиеся на это имя, перепривяжутся к новому хэндлу.
	input_manager->RegisterCommand(CommandId::UpsertTexture,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertTextureCmd* c = static_cast<const UpsertTextureCmd*>(data);
			if (!c->name.empty() && !c->atlas.empty() && !c->path.empty()) {
				ctx->GetTextureManager()->DeleteTextureHandle(c->name);   // no-op, если имени нет
				ctx->CreateTextureFromFile(c->name, c->atlas, c->path.c_str(), static_cast<ChannelConvention>(c->conv));
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Новый материал: sp "sp" (main) + дефолт-текстуры по его required_slots + дефолт-params.
	// Имя приходит из UI (уже свободное); если вдруг занято — CreateMaterial вернёт существующий.
	input_manager->RegisterCommand(CommandId::CreateMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const CreateMaterialCmd* c = static_cast<const CreateMaterialCmd*>(data);
			ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram("sp");
			std::vector<std::pair<TextureSlotRole, TextureName>> texs;
			if (sp) for (TextureSlotRole role : sp->required_slots) texs.emplace_back(role, DefaultTextureForRole(role));
			Material* m = ctx->GetMaterialManager()->CreateMaterial(c->name, std::move(texs), std::vector<ShaderName>{ "sp" });
			if (m) ctx->SetMaterialParams(m, OpaqueMaterialParams{});   // sp несёт MaterialBlock → нужен блоб
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Добавить sp материалу: дописать имя (если ещё нет) + добрать дефолтами ТОЛЬКО новые роли
	// (общие с другими sp не трогаем — текстура роли шарится).
	input_manager->RegisterCommand(CommandId::AddMaterialShader,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialShaderCmd* c = static_cast<const MaterialShaderCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				bool present = false;
				for (auto& s : m->shader_programs) if (s == c->shader) { present = true; break; }
				if (!present) {
					m->shader_programs.push_back(c->shader);
					if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c->shader))
						for (TextureSlotRole role : sp->required_slots)
							if (!m->textures.count(role)) m->textures[role] = DefaultTextureForRole(role);
					ctx->GetBatchBuilder()->SetDirtyBatches(true);
				}
			}
			delete c;
		});

	// Убрать sp у материала (leftover-роли в textures не чистим — безвредны, просто не используются).
	input_manager->RegisterCommand(CommandId::RemoveMaterialShader,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialShaderCmd* c = static_cast<const MaterialShaderCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				auto& sps = m->shader_programs;
				for (size_t i = 0; i < sps.size(); ++i) if (sps[i] == c->shader) { sps.erase(sps.begin() + i); break; }
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Переименование материала — ре-кей в словаре + пересборка (материалы резолвятся по имени).
	input_manager->RegisterCommand(CommandId::RenameMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const RenameMaterialCmd* c = static_cast<const RenameMaterialCmd*>(data);
			if (ctx->GetMaterialManager()->RenameMaterial(c->oldName, c->newName))
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
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
