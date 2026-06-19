#include "PCH.h"
#include "RenderManager.h"
#include "BufferManager.h"   // DefaultBuffersNames + BufferManager (через параметр)
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
	// Разделяемый depth резолвим в момент begin: после ресайза таргет уже держит новую
	// текстуру, поэтому все проходы подхватывают её автоматически, без переназначения.
	auto& tex_data = render_pass_step->renderPassTexsData;
	if (tex_data.shared_depth)
		tex_data.depthTargetInfo.texture = tex_data.shared_depth->texture;

	SDL_GPURenderPass* rp = nullptr;
	rp = SDL_BeginGPURenderPass(cb,
		&tex_data.colorTargetInfo,
		tex_data.numColorTargets,
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
			shader_batch.dispatch_func(dispatch_binder, dispatch_data_raw);
			elements = dispatch_binder.element_count;
		}

		if (elements.x == 0 || elements.y == 0 || elements.z == 0) continue;

		if (shader_batch.push_func) {
			PushConstantBinder binder{ cb };
			shader_batch.push_func(binder, push_data_raw);
		}

		std::vector<SDL_GPUStorageBufferReadWriteBinding> storage_buffer_bindings =
			bm->BuildBindGPUComputeRWBuffers(shader_batch.rw_storage_buffers, pass_frame);

		SDL_GPUComputePass* cmp = SDL_BeginGPUComputePass(cb,
			shader_batch.rw_storage_textures.data(), safe_u32(shader_batch.rw_storage_textures.size()),
			storage_buffer_bindings.data(), safe_u32(storage_buffer_bindings.size()));

		SDL_BindGPUComputePipeline(cmp, shader_batch.pipeline);
		if (!shader_batch.texture_binding.empty()) {
			SDL_BindGPUComputeSamplers(cmp, 0,
				shader_batch.texture_binding.data(), safe_u32(shader_batch.texture_binding.size()));
		}
		if (!shader_batch.ro_storage_textures.empty()) {
			SDL_BindGPUComputeStorageTextures(cmp, 0,
				shader_batch.ro_storage_textures.data(), safe_u32(shader_batch.ro_storage_textures.size()));
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
	if (it != compute_steps.end()) {
		return it->second.get();
	}
	SDL_Log("RenderManager::Compute pass '%s' not found", name.c_str());
	return nullptr;
}

ComputePassStep* PassManager::GetComputePrepassStep(const ComputePrepassName& name)
{
	auto it = compute_prepass_steps.find(name);
	if (it != compute_prepass_steps.end()) {
		return it->second.get();
	}
	SDL_Log("Compute prepass '%s' not found", name.c_str());
	return nullptr;
}

PassManager::~PassManager()
{
	render_steps.clear();
}

inline void PassManager::ExecuteRenderBatches(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* rp, const RenderPassStep& render_pass_step, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw)
{
	int draw_calls = 0;
	for (auto& [_, shader_batch] : render_pass_step.shader_batches)
	{
		SDL_BindGPUGraphicsPipeline(rp, shader_batch.pipeline);
		SDL_BindGPUFragmentSamplers(rp, 0, render_pass_step.global_texture_bindings.data(), safe_u32(render_pass_step.global_texture_bindings.size()));

		bm->BindGPUVertexBuffer(rp, 0, 0);
		bm->BindGPUIndexBuffer(rp, 0);

		PushConstantBinder binder{ cb };
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

		for (auto& [_, atlas_batch] : shader_batch.atlases_batches) {
			if (!atlas_batch.texture_binding.empty()) {
				SDL_BindGPUFragmentSamplers(rp, safe_u32(render_pass_step.global_texture_bindings.size()), atlas_batch.texture_binding.data(), safe_u32(atlas_batch.texture_binding.size()));
			}
			for (auto& [_, texture_batch] : atlas_batch.texture_batches) {
				if (!texture_batch.texture_uvl.empty()) {
					TextureData padded[4] = {};
					size_t count = std::min(texture_batch.texture_uvl.size(), size_t(4));
					for (size_t i = 0; i < count; i++) {
						if (texture_batch.texture_uvl[i])
							padded[i] = *texture_batch.texture_uvl[i];
					}
					// .x/.y/.z альбедо (offset/scale/layer) уже скопированы выше; здесь
					// дописываем ТОЛЬКО неиспользуемый .w (_pad) — туда кладём биты
					// per-material альфы. Шейдер читает её как asfloat(textures[0].data.w).
					if (count > 0)
						SDL_memcpy(&padded[0]._pad, &texture_batch.alpha, sizeof(float));
					SDL_PushGPUFragmentUniformData(cb, uvl_slot, padded, sizeof(padded));
				}
				else {
					//SDL_Log("Texture batch UVL data does not exist or is empty");
				}

				SDL_DrawGPUIndexedPrimitivesIndirect(rp,
					bm->_GetGPUBufferForFrame(bm->GetBufferData(DefaultBuffersNames::DEFAULT_INDIRECT_BUFFER), render_frame),
					safe_u32(additional_offset +
						texture_batch.indirect_command_index * sizeof(SDL_GPUIndexedIndirectDrawCommand)),
					safe_u32(texture_batch.model_batches.size())
				);
				draw_calls++;

				
			}
		}
	}
	//std::cout << "Draw calls: " << draw_calls << " for pass" << render_pass_step.pass_index << std::endl;
}
