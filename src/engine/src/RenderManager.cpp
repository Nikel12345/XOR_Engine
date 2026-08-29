#include "PCH.h"
#include "RenderManager.h"
#include "BufferManager.h"
#include "RenderSnapshot.h"
// MaterialManager/PipeManager/ObjectManager/ModelData здесь не использовались — убраны,
// чтобы PassManager не тянул render/model/object на уровне линковки (нужно для EngineGpu).

PassManager::PassManager() {}

RenderPassStep* PassManager::CreateRenderPass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function, RenderPassTexturesInfo&& rptd, int pass_index)
{
	if (pass_index == -1) {
		SDL_Log("PassManager::CreateRenderPass: Invalid RenderPassStep order index.");
		return nullptr;
	}
	auto it_pass = render_steps.find(name);
	if (it_pass != render_steps.end()) {
		SDL_Log("PassManager::CreateRenderPass: Render pass with name '%s' already exists.", name.c_str());
		return it_pass->second.get();
	}
	auto data = std::make_unique<RenderPassStep>();
	data->renderPassTexsData = std::move(rptd);
	data->render_function = render_function;
	data->debug_name = name;
	data->pass_index = pass_index;

	RenderPassStep* ptr = data.get();
	render_steps[name] = std::move(data);

	return ptr;
}

ComputePassStep* PassManager::CreateComputePass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index)
 {
	if (pass_index == -1) {
		SDL_Log("PassManager::CreateComputePass: Invalid ComputePassStep order index.");
		return nullptr;
	}
	auto it_pass = compute_steps.find(name);
	if (it_pass != compute_steps.end()) {
		SDL_Log("PassManager::CreateComputePass: Compute pass with name '%s' already exists.", name.c_str());
		return it_pass->second.get();
	}
	auto it_prepass = compute_prepass_steps.find(name);
	if (it_prepass != compute_prepass_steps.end()) {
		SDL_Log("PassManager::CreateComputePass: A compute prepass with name '%s' already exists. Cannot create a pass with the same name.", name.c_str());
		return nullptr;
	}
	auto data = std::make_unique<ComputePassStep>();
	data->compute_function = compute_function;
	data->pass_index = pass_index;
	data->debug_name = name;

	ComputePassStep* ptr = data.get();
	compute_steps[name] = std::move(data);
	return ptr;
}

ComputePassStep* PassManager::CreateComputePrepass(const ComputePrepassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, int pass_index)
{
	if (pass_index == -1) {
		SDL_Log("PassManager::CreateComputePrepass: Invalid ComputePrepassStep order index.");
		return nullptr;
	}
	auto it_prepass = compute_prepass_steps.find(name);
	if (it_prepass != compute_prepass_steps.end()) {
		SDL_Log("PassManager::CreateComputePrepass: Compute prepass with name '%s' already exists.", name.c_str());
		return it_prepass->second.get();
	}
	auto it_pass = compute_steps.find(name);
	if (it_pass != compute_steps.end()) {
		SDL_Log("PassManager::CreateComputePrepass: A compute pass with name '%s' already exists. Cannot create a prepass with the same name.", name.c_str());
		return nullptr;
	}
	auto data = std::make_unique<ComputePassStep>();
	data->compute_function = compute_function;
	data->pass_index = pass_index;
	data->debug_name = name;

	ComputePassStep* ptr = data.get();
	compute_prepass_steps[name] = std::move(data);
	return ptr;
}

BlitPassStep* PassManager::CreateBlitPass(const BlitPassName& name, TextureAtlas* src, TextureAtlas* dst, int pass_index,
	SDL_GPUFilter filter, SDL_GPULoadOp load_op)
{
	if (pass_index == -1) {
		SDL_Log("PassManager::CreateBlitPass: Invalid BlitPassStep order index.");
		return nullptr;
	}
	if (!src || !dst) {
		SDL_Log("PassManager::CreateBlitPass: '%s' - src/dst atlas is null.", name.c_str());
		return nullptr;
	}
	auto it_blit = blit_steps.find(name);
	if (it_blit != blit_steps.end()) {
		SDL_Log("PassManager::CreateBlitPass: Blit pass with name '%s' already exists.", name.c_str());
		return it_blit->second.get();
	}
	// Имя прохода — общее пространство: sp/UI ищут проход по имени, дубль между видами
	// сделал бы поиск неоднозначным.
	if (render_steps.count(name) || compute_steps.count(name) || compute_prepass_steps.count(name)) {
		SDL_Log("PassManager::CreateBlitPass: A pass with name '%s' already exists (render/compute). Cannot create a blit pass with the same name.", name.c_str());
		return nullptr;
	}

	// Декларация usage: SDL требует у источника блита SAMPLER, у назначения — COLOR_TARGET
	// (проверено зондом sandbox/BlitUsageProbe.cpp; в докстрингах SDL этого нет).
	// Свопчейн-атлас движок не создаёт — флаги ему безразличны, но union'у это не мешает.
	src->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
	dst->tci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

	auto data = std::make_unique<BlitPassStep>();
	data->src = src;
	data->dst = dst;
	data->filter = filter;
	data->load_op = load_op;
	data->debug_name = name;
	data->pass_index = pass_index;

	BlitPassStep* ptr = data.get();
	blit_steps[name] = std::move(data);
	return ptr;
}

void PassManager::SetSwapchain(SDL_GPUTexture* tex, uint32_t w, uint32_t h)
{
	swapchain_atlas.texture_binding.texture = tex;
	swapchain_atlas.width = w;
	swapchain_atlas.height = h;
}

void PassManager::ResolveAllTextureTargets()
{
	for (int i = 0; i < ordered_passes.size(); i++) {
		ordered_passes[i]->renderPassTexsData.ResolveTargets();
	}
}

void PassManager::FillRenderPasses()
{
	if (passes_filled) {
		SDL_Log("PassManager::FillRenderPasses: Passes are already filled.");
		return;
	}
	ordered_passes.clear();
	ordered_passes.reserve(render_steps.size());
	for (auto& [_, rp] : render_steps)
		ordered_passes.push_back(rp.get());

	ordered_compute_steps.clear();
	ordered_compute_steps.reserve(compute_steps.size());
	for (auto& [_, cs] : compute_steps)
		ordered_compute_steps.push_back(cs.get());

	ordered_compute_prepass_steps.clear();
	ordered_compute_prepass_steps.reserve(compute_prepass_steps.size());
	for (auto& [_, pcs] : compute_prepass_steps)
		ordered_compute_prepass_steps.push_back(pcs.get());

	ordered_blit_steps.clear();
	ordered_blit_steps.reserve(blit_steps.size());
	for (auto& [_, bs] : blit_steps)
		ordered_blit_steps.push_back(bs.get());

	std::sort(ordered_passes.begin(), ordered_passes.end(),
		[](const RenderPassStep* a, const RenderPassStep* b) {
		return a->pass_index < b->pass_index;
	});

	std::sort(ordered_blit_steps.begin(), ordered_blit_steps.end(),
		[](const BlitPassStep* a, const BlitPassStep* b) {
		return a->pass_index < b->pass_index;
	});

	std::sort(ordered_compute_steps.begin(), ordered_compute_steps.end(),
		[](const ComputePassStep* a, const ComputePassStep* b) {
		return a->pass_index < b->pass_index;
	});

	std::sort(ordered_compute_prepass_steps.begin(), ordered_compute_prepass_steps.end(),
		[](const ComputePassStep* a, const ComputePassStep* b) {
		return a->pass_index < b->pass_index;
	});

	// Ordinal = индекс прохода в BatchLayout::passes (слепок строится обходом ordered_passes
	// в этом же порядке). Ставится один раз — проходы после старта не добавляются.
	// Блит-проходы сюда НЕ входят: батчей у них нет, рисовать в них нечем.
	for (size_t i = 0; i < ordered_passes.size(); ++i)
		ordered_passes[i]->ordinal = safe_u32(i);

	// Единый порядок кадра. Push'им render → compute → blit, затем СТАБИЛЬНАЯ сортировка:
	// при равном pass_index сохраняется этот же приоритет (раньше ExecutePassesSteps давала
	// render'у идти первым на равенстве — поведение сохранено).
	ordered_execution.clear();
	ordered_execution.reserve(ordered_passes.size() + ordered_compute_steps.size() + ordered_blit_steps.size());
	for (RenderPassStep* rp : ordered_passes)         ordered_execution.push_back({ rp->pass_index, rp });
	for (ComputePassStep* cs : ordered_compute_steps) ordered_execution.push_back({ cs->pass_index, cs });
	for (BlitPassStep* bs : ordered_blit_steps)       ordered_execution.push_back({ bs->pass_index, bs });
	std::stable_sort(ordered_execution.begin(), ordered_execution.end(),
		[](const OrderedStep& a, const OrderedStep& b) {
		return a.pass_index < b.pass_index;
	});

	passes_filled = true;
}

void PassManager::ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame)
{
	// Порядок слит один раз в FillRenderPasses (ordered_execution) — здесь просто идём по нему.
	for (const OrderedStep& step : ordered_execution) {
		std::visit([&](auto* pass) {
			using T = std::remove_pointer_t<decltype(pass)>;
			if constexpr (std::is_same_v<T, RenderPassStep>)       pass->render_function(cb, this, *pass);
			else if constexpr (std::is_same_v<T, ComputePassStep>) pass->compute_function(cb, this, *pass, pass_frame);
			else if constexpr (std::is_same_v<T, BlitPassStep>)    BlitPassStandardBody(cb, *pass);
		}, step.step);
	}
}

void PassManager::BlitPassStandardBody(SDL_GPUCommandBuffer* cb, BlitPassStep& bp)
{
	if (!bp.src || !bp.dst) return;

	// Текстуры резолвим ЗДЕСЬ, а не на создании: у HDR-таргетов её подменяет ресайз, у
	// свопчейн-атласа — каждый кадр (Engine::RenderFunc). Указатели на сами атласы стабильны.
	SDL_GPUTexture* src_tex = bp.src->texture_binding.texture;
	SDL_GPUTexture* dst_tex = bp.dst->texture_binding.texture;
	// dst — свопчейн, а кадр без свопчейна: текстуры нет → блит пропускаем (это не ошибка).
	if (!src_tex || !dst_tex) return;
	if (bp.src->width == 0 || bp.src->height == 0 || bp.dst->width == 0 || bp.dst->height == 0) return;

	SDL_GPUBlitInfo bi{};
	bi.source.texture = src_tex;
	bi.source.mip_level = bp.src_mip;
	bi.source.layer_or_depth_plane = bp.src_layer;
	bi.source.w = bp.src->width;    // размеры — у КАЖДОГО свои (src из атласа, dst из свопчейна):
	bi.source.h = bp.src->height;   // совпадать они не обязаны, blit сам масштабирует и конвертирует формат
	bi.destination.texture = dst_tex;
	bi.destination.w = bp.dst->width;
	bi.destination.h = bp.dst->height;
	bi.load_op = bp.load_op;
	bi.filter = bp.filter;
	bi.cycle = false;
	SDL_BlitGPUTexture(cb, &bi);
}

void PassManager::ExecutePrepassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame)
{
	for (auto& step : ordered_compute_prepass_steps) {
		step->compute_function(cb, this, *step, pass_frame);
	}
}

void PassManager::RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass_step, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw)
{
	auto& tex_data = render_pass_step->renderPassTexsData;
	// Атласы/shared-depth → актуальные текстуры (создаёт бейк, подменяет ресайз).

	SDL_GPURenderPass* rp = nullptr;
	rp = SDL_BeginGPURenderPass(cb,
		tex_data.colorTargetInfos.data(),
		safe_u32(tex_data.colorTargetInfos.size()),
		&tex_data.depthTargetInfo);
	if (!rp) {
		SDL_Log("PassManager::ExecutePassesSteps: Failed to begin render pass!");
		return;
	}
	ExecuteRenderBatches(cb, rp, *render_pass_step, bm, additional_offset, push_data_raw);
	SDL_EndGPURenderPass(rp);
	
}

void PassManager::ComputePassStandardBody(SDL_GPUCommandBuffer* cb, ComputePassStep* compute_step,
	BufferManager* bm, const void* push_data_raw, const void* dispatch_data_raw, uint8_t pass_frame)
{
	for (const auto& shader_batch : compute_step->shader_batches) {
		glm::uvec3 elements{ 1, 1, 1 };
		if (shader_batch.dispatch_func) {
			DispatchSizeBinder dispatch_binder{};
			dispatch_binder.slot = pass_frame;   // ключ пер-слотовых слепков для Ask*(slot)
			shader_batch.dispatch_func(dispatch_binder, dispatch_data_raw);
			elements = dispatch_binder.element_count;
		}

		if (elements.x == 0 || elements.y == 0 || elements.z == 0) continue;

		if (shader_batch.push_func) {
			PushConstantBinder binder{ cb, pass_frame };
			shader_batch.push_func(binder, push_data_raw);
		}

		std::vector<SDL_GPUStorageBufferReadWriteBinding> storage_buffer_bindings =
			bm->BuildBindGPUComputeRWBuffers(shader_batch.rw_storage_buffers, pass_frame);

		// Резолвим СТАБИЛЬНЫЕ атласы в актуальные SDL-биндинги ЗДЕСЬ (на момент диспатча): после
		// ресайза атлас уже держит новую текстуру, поэтому батч пересобирать не нужно. SDL копирует
		// массивы при вызове, поэтому локальные временные векторы безопасны.
		std::vector<SDL_GPUStorageTextureReadWriteBinding> rw_textures;
		rw_textures.reserve(shader_batch.rw_storage_textures.size());
		for (const auto& r : shader_batch.rw_storage_textures)
			rw_textures.push_back({ r.atlas->texture_binding.texture, r.mip_level, r.layer, false });

		SDL_GPUComputePass* cmp = SDL_BeginGPUComputePass(cb,
			rw_textures.data(), safe_u32(rw_textures.size()),
			storage_buffer_bindings.data(), safe_u32(storage_buffer_bindings.size()));

		SDL_BindGPUComputePipeline(cmp, shader_batch.pipeline);
		if (!shader_batch.texture_binding.empty()) {
			std::vector<SDL_GPUTextureSamplerBinding> samplers;
			samplers.reserve(shader_batch.texture_binding.size());
			for (TextureAtlas* a : shader_batch.texture_binding)
				samplers.push_back(a->texture_binding);
			SDL_BindGPUComputeSamplers(cmp, 0, samplers.data(), safe_u32(samplers.size()));
		}
		if (!shader_batch.ro_storage_textures.empty()) {
			std::vector<SDL_GPUTexture*> ro_textures;
			ro_textures.reserve(shader_batch.ro_storage_textures.size());
			for (TextureAtlas* a : shader_batch.ro_storage_textures)
				ro_textures.push_back(a->texture_binding.texture);
			SDL_BindGPUComputeStorageTextures(cmp, 0, ro_textures.data(), safe_u32(ro_textures.size()));
		}
		if (!shader_batch.ro_storage_buffers.empty()) {
			bm->BindGPUComputeRO_Buffers(cmp, 0, shader_batch.ro_storage_buffers, pass_frame);
		}

		const uint32_t gx = (elements.x + shader_batch.threadcount_x - 1) / shader_batch.threadcount_x;
		const uint32_t gy = (elements.y + shader_batch.threadcount_y - 1) / shader_batch.threadcount_y;
		const uint32_t gz = (elements.z + shader_batch.threadcount_z - 1) / shader_batch.threadcount_z;
		SDL_DispatchGPUCompute(cmp, gx, gy, gz);

		SDL_EndGPUComputePass(cmp);
	}
}

RenderPassStep* PassManager::GetRenderPassStep(const RenderPassName& name)
{
	auto it = render_steps.find(name);
	if (it != render_steps.end()) {
		return it->second.get();
	}
	SDL_Log("RenderManager::Render pass '%s' not found", name.c_str());
	return nullptr;
}

ComputePassStep* PassManager::GetComputePassStep(const ComputePassName& name)
{
	auto it = compute_steps.find(name);
	return (it != compute_steps.end()) ? it->second.get() : nullptr;
}

ComputePassStep* PassManager::GetComputePrepassStep(const ComputePrepassName& name)
{
	auto it = compute_prepass_steps.find(name);
	return (it != compute_prepass_steps.end()) ? it->second.get() : nullptr;
}

BlitPassStep* PassManager::GetBlitPassStep(const BlitPassName& name)
{
	auto it = blit_steps.find(name);
	return (it != blit_steps.end()) ? it->second.get() : nullptr;
}

PassManager::~PassManager()
{
	render_steps.clear();
}

// Рисует по СЛЕПКУ раскладки рендеримого слота (render_layout), не по живому дереву:
// офсеты/uvl/draw_count гарантированно соответствуют indirect_buffer[render_frame],
// залитому тем же prepare, а sim может свободно перестраивать дерево параллельно.
inline void PassManager::ExecuteRenderBatches(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* rp, const RenderPassStep& render_pass_step, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw)
{
	if (!render_layout || render_pass_step.ordinal >= render_layout->passes.size()) return;
	const RenderSnap::PassDrawList& pass_list = render_layout->passes[render_pass_step.ordinal];

	// Индирект-буфер раскладки — из слепка (BufferData* отрезолвлен в FinalizeOffsets); здесь
	// только пер-кадровый хэндл. Нет буфера — рисовать нечем (все дроу этого слепка — indirect).
	SDL_GPUBuffer* indirect_buf = bm->_GetGPUBufferForFrame(render_layout->indirectBuffer, render_frame);
	if (!indirect_buf) {
		SDL_Log("ExecuteRenderBatches: indirect buffer is missing - pass draw list skipped");
		return;
	}

	// Глобальные сэмплеры прохода — из СЛЕПКА (отрезолвлены на сборке батча), не из живого
	// прохода: их GPU-текстуры могут пересоздаваться, а рендер обязан видеть согласованный слот.
	const std::vector<SDL_GPUTextureSamplerBinding>& global_samplers = pass_list.global_texture_bindings;
	const uint32_t global_sampler_count = safe_u32(global_samplers.size());

	int draw_calls = 0;
	for (const RenderSnap::ShaderGroup& shader_batch : pass_list.shaders)
	{
		SDL_BindGPUGraphicsPipeline(rp, shader_batch.pipeline);
		SDL_BindGPUFragmentSamplers(rp, 0, global_samplers.data(), global_sampler_count);

		// Вершинные стримы — список из объявления vs (слепок), порядок = слоты пайплайна.
		// Сбой бинда = ПРОПУСК шейдер-батча целиком: пайплайн ждёт в слоте k страйд стрима k,
		// рисовать с несбинженными/сдвинутыми слотами — UB, а не деградация.
		if (!bm->BindGPUVertexBuffers(rp, shader_batch.vertexBuffers)) {
			SDL_Log("ExecuteRenderBatches: vertex stream bind failed - shader batch skipped");
			continue;
		}
		// Индексный буфер пула батча — из слепка (та же дисциплина, что у стримов).
		if (!bm->BindGPUIndexBuffer(rp, shader_batch.indexBuffer, 0)) {
			SDL_Log("ExecuteRenderBatches: index buffer bind failed - shader batch skipped");
			continue;
		}

		PushConstantBinder binder{ cb, render_frame };
		if (shader_batch.push_func) {
			shader_batch.push_func(binder, push_data_raw);
		}
		const uint32_t uvl_slot = binder.frag_count;

		if (!shader_batch.vertexStorageBuffers.empty()) {
			bm->BindGPUVertexStorageBuffers(rp, 0, shader_batch.vertexStorageBuffers, render_frame);
		}
		else {
		}
		if (!shader_batch.fragmentStorageBuffers.empty()) {
			bm->BindGPUFragmentStorageBuffers(rp, 0, shader_batch.fragmentStorageBuffers, render_frame);
		}
		else {
		}

		for (const RenderSnap::AtlasGroup& atlas_batch : shader_batch.atlases) {
			if (!atlas_batch.texture_binding.empty()) {
				// Батчевые сэмплеры идут ПОСЛЕ глобальных — база слота = их число.
				SDL_BindGPUFragmentSamplers(rp, global_sampler_count, atlas_batch.texture_binding.data(), safe_u32(atlas_batch.texture_binding.size()));
			}
			for (const RenderSnap::TextureDraw& texture_batch : atlas_batch.draws) {
				if (!texture_batch.texture_uvl.empty()) {
					SDL_PushGPUFragmentUniformData(cb, uvl_slot,
						texture_batch.texture_uvl.data(),
						safe_u32(texture_batch.texture_uvl.size() * sizeof(UVL_Block)));
				}

				if (texture_batch.params && !texture_batch.params->empty()
						/* пушим, ТОЛЬКО если шейдер объявил uniform на этом слоте (нет MaterialBlock у shadow/depth → не лезем) */
						&& (uvl_slot + (texture_batch.texture_uvl.empty() ? 0u : 1u)) < shader_batch.frag_uniform_count) {
					SDL_PushGPUFragmentUniformData(cb, uvl_slot + (texture_batch.texture_uvl.empty() ? 0u : 1u),   /* params: плотный слот (без UVL → uvl_slot) */
						texture_batch.params->data(), safe_u32(texture_batch.params->size()));
				}

				// Раскладка таблицы UVL — ТРЕТИЙ fragment-uniform, ПОСЛЕ params: так
				// MATERIAL_BLOCK_REGISTER не двигается (b0 uvl, b1 params, b2 раскладка).
				// Гейт тот же, что у params: пушим, только если шейдер объявил uniform на этом
				// слоте (у shadow/wireframe/untextured его нет — туда не лезем).
				if (uvl_slot + 2 < shader_batch.frag_uniform_count) {
					SDL_PushGPUFragmentUniformData(cb, uvl_slot + 2,
						&texture_batch.variant_layout, sizeof(VariantLayout));
				}

				SDL_DrawGPUIndexedPrimitivesIndirect(rp,
					indirect_buf,
					safe_u32(additional_offset +
						texture_batch.indirect_command_index * sizeof(SDL_GPUIndexedIndirectDrawCommand)),
					texture_batch.draw_count
				);
				draw_calls++;


			}
		}
	}
}
