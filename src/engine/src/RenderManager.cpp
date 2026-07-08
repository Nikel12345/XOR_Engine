#include "PCH.h"
#include "RenderManager.h"
#include "BufferManager.h"   // DefaultBuffersNames + BufferManager (через параметр)
#include "RenderSnapshot.h"  // header-only типы слепков (линковку не расширяет)
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

	ComputePassStep* ptr = data.get();
	compute_prepass_steps[name] = std::move(data);
	return ptr;
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

	std::sort(ordered_passes.begin(), ordered_passes.end(),
		[](const RenderPassStep* a, const RenderPassStep* b) {
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
	for (size_t i = 0; i < ordered_passes.size(); ++i)
		ordered_passes[i]->ordinal = safe_u32(i);

	passes_filled = true;
}

void PassManager::ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame)
{
	int i = 0, j = 0;
	while (i < ordered_passes.size() || j < ordered_compute_steps.size()) {
		if (i < ordered_passes.size() && (j >= ordered_compute_steps.size() || ordered_passes[i]->pass_index <= ordered_compute_steps[j]->pass_index)) {
			ordered_passes[i]->render_function(cb, this, *ordered_passes[i]);
			i++;
		}
		else if (j < ordered_compute_steps.size()) {
			ordered_compute_steps[j]->compute_function(cb, this, *ordered_compute_steps[j], pass_frame);
			j++;
		}
	}
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
	if (tex_data.shared_depth)
		tex_data.depthTargetInfo.texture = tex_data.shared_depth->texture;

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

	int draw_calls = 0;
	for (const RenderSnap::ShaderGroup& shader_batch : pass_list.shaders)
	{
		SDL_BindGPUGraphicsPipeline(rp, shader_batch.pipeline);
		SDL_BindGPUFragmentSamplers(rp, 0, render_pass_step.global_texture_bindings.data(), safe_u32(render_pass_step.global_texture_bindings.size()));

		bm->BindGPUVertexBuffer(rp, 0, 0);
		bm->BindGPUIndexBuffer(rp, 0);

		PushConstantBinder binder{ cb, render_frame };
		if (shader_batch.push_func) {
			shader_batch.push_func(binder, push_data_raw);
		}
		const uint32_t uvl_slot = binder.frag_count;

		if (!shader_batch.vertexStorageBuffers.empty()) {
			bm->BindGPUVertexStorageBuffers(rp, 0, shader_batch.vertexStorageBuffers, render_frame);
		}
		else {
			//SDL_Log("No vertex storage buffers found for render command");
		}
		if (!shader_batch.fragmentStorageBuffers.empty()) {
			bm->BindGPUFragmentStorageBuffers(rp, 0, shader_batch.fragmentStorageBuffers, render_frame);
		}
		else {
			//SDL_Log("No fragment storage buffers found for render command");
		}

		for (const RenderSnap::AtlasGroup& atlas_batch : shader_batch.atlases) {
			if (!atlas_batch.texture_binding.empty()) {
				SDL_BindGPUFragmentSamplers(rp, safe_u32(render_pass_step.global_texture_bindings.size()), atlas_batch.texture_binding.data(), safe_u32(atlas_batch.texture_binding.size()));
			}
			for (const RenderSnap::TextureDraw& texture_batch : atlas_batch.draws) {
				if (!texture_batch.texture_uvl.empty()) {
					SDL_PushGPUFragmentUniformData(cb, uvl_slot,
						texture_batch.texture_uvl.data(),
						safe_u32(texture_batch.texture_uvl.size() * sizeof(TextureData)));
				}

				if (texture_batch.params && !texture_batch.params->empty()
						/* пушим, ТОЛЬКО если шейдер объявил uniform на этом слоте (нет MaterialBlock у shadow/depth → не лезем) */
						&& (uvl_slot + (texture_batch.texture_uvl.empty() ? 0u : 1u)) < shader_batch.frag_uniform_count) {
					SDL_PushGPUFragmentUniformData(cb, uvl_slot + (texture_batch.texture_uvl.empty() ? 0u : 1u),   /* params: плотный слот (без UVL → uvl_slot) */
						texture_batch.params->data(), safe_u32(texture_batch.params->size()));
				}

				SDL_DrawGPUIndexedPrimitivesIndirect(rp,
					bm->_GetGPUBufferForFrame(bm->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER), render_frame),
					safe_u32(additional_offset +
						texture_batch.indirect_command_index * sizeof(SDL_GPUIndexedIndirectDrawCommand)),
					texture_batch.draw_count
				);
				draw_calls++;


			}
		}
	}
	//std::cout << "Draw calls: " << draw_calls << " for pass" << render_pass_step.pass_index << std::endl;
}
