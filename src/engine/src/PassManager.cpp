#include "PCH.h"
#include "PassManager.h"
#include "BufferManager.h"
#include "RenderSnapshot.h"

PassManager::PassManager() {}

// Пассы всех видов и препассы делят ОДНО пространство имён. Якорь называет проход одним именем,
// без вида. Compute-программа тоже называет свой проход одним именем (compute_pass_name), а
// BatchBuilder на сборке ищет его сначала среди пассов, потом среди препассов. Одноимённые увели бы
// якорь или программу в чужой проход — молча, потому что оба варианта валидны.
bool PassManager::IsPassNameTaken(const std::string& name) const
{
	return render_steps.count(name) || compute_steps.count(name)
		|| compute_prepass_steps.count(name) || blit_steps.count(name);
}

bool PassManager::CanCreatePass(const std::string& name, const char* caller) const
{
	if (passes_filled) {
		SDL_Log("%s: pass '%s' is not created - passes are already filled.", caller, name.c_str());
		return false;
	}
	if (IsPassNameTaken(name)) {
		SDL_Log("%s: name '%s' is already taken by a pass of another kind.", caller, name.c_str());
		return false;
	}
	return true;
}

std::optional<FrameStep> PassManager::FindFrameStep(const std::string& name) const
{
	if (auto it = render_steps.find(name); it != render_steps.end())   return FrameStep{ it->second.get() };
	if (auto it = compute_steps.find(name); it != compute_steps.end()) return FrameStep{ it->second.get() };
	if (auto it = blit_steps.find(name); it != blit_steps.end())       return FrameStep{ it->second.get() };
	return std::nullopt;
}

std::optional<std::list<FrameStep>::iterator> PassManager::FrameInsertPosition(const PassAnchor& anchor, const std::string& name, const char* caller)
{
	if (!anchor.after_pass) return ordered_execution.begin();
	const std::optional<FrameStep> anchor_step = FindFrameStep(*anchor.after_pass);
	if (!anchor_step) {
		SDL_Log("%s: pass '%s' is not created - anchor pass '%s' is not in the frame chain.",
			caller, name.c_str(), anchor.after_pass->c_str());
		return std::nullopt;
	}
	return std::next(std::find(ordered_execution.begin(), ordered_execution.end(), *anchor_step));
}

std::optional<std::list<ComputePassStep*>::iterator> PassManager::PrepassInsertPosition(const PassAnchor& anchor, const std::string& name, const char* caller)
{
	if (!anchor.after_pass) return ordered_compute_prepass_steps.begin();
	auto anchor_prepass = compute_prepass_steps.find(*anchor.after_pass);
	if (anchor_prepass == compute_prepass_steps.end()) {
		SDL_Log("%s: prepass '%s' is not created - anchor prepass '%s' is not in the prepass chain.",
			caller, name.c_str(), anchor.after_pass->c_str());
		return std::nullopt;
	}
	return std::next(std::find(ordered_compute_prepass_steps.begin(), ordered_compute_prepass_steps.end(), anchor_prepass->second.get()));
}

RenderPassStep* PassManager::CreateRenderPass(const RenderPassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, RenderPassStep&)> render_function, RenderPassTexturesInfo&& rptd, const PassAnchor& anchor)
{
	auto it_pass = render_steps.find(name);
	if (it_pass != render_steps.end()) {
		SDL_Log("PassManager::CreateRenderPass: Render pass with name '%s' already exists.", name.c_str());
		return it_pass->second.get();
	}
	if (!CanCreatePass(name, "PassManager::CreateRenderPass")) return nullptr;
	const auto insert_position = FrameInsertPosition(anchor, name, "PassManager::CreateRenderPass");
	if (!insert_position) return nullptr;

	auto data = std::make_unique<RenderPassStep>();
	data->renderPassTexsData = std::move(rptd);
	data->render_function = render_function;
	data->debug_name = name;

	RenderPassStep* ptr = data.get();
	render_steps[name] = std::move(data);
	ordered_execution.insert(*insert_position, ptr);
	return ptr;
}

ComputePassStep* PassManager::CreateComputePass(const ComputePassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, const PassAnchor& anchor)
{
	auto it_pass = compute_steps.find(name);
	if (it_pass != compute_steps.end()) {
		SDL_Log("PassManager::CreateComputePass: Compute pass with name '%s' already exists.", name.c_str());
		return it_pass->second.get();
	}
	if (!CanCreatePass(name, "PassManager::CreateComputePass")) return nullptr;
	const auto insert_position = FrameInsertPosition(anchor, name, "PassManager::CreateComputePass");
	if (!insert_position) return nullptr;

	auto data = std::make_unique<ComputePassStep>();
	data->compute_function = compute_function;
	data->debug_name = name;

	ComputePassStep* ptr = data.get();
	compute_steps[name] = std::move(data);
	ordered_execution.insert(*insert_position, ptr);
	return ptr;
}

ComputePassStep* PassManager::CreateComputePrepass(const ComputePrepassName& name, std::function<void(SDL_GPUCommandBuffer*, PassManager*, ComputePassStep&, uint8_t)> compute_function, const PassAnchor& anchor)
{
	auto it_prepass = compute_prepass_steps.find(name);
	if (it_prepass != compute_prepass_steps.end()) {
		SDL_Log("PassManager::CreateComputePrepass: Compute prepass with name '%s' already exists.", name.c_str());
		return it_prepass->second.get();
	}
	if (!CanCreatePass(name, "PassManager::CreateComputePrepass")) return nullptr;
	const auto insert_position = PrepassInsertPosition(anchor, name, "PassManager::CreateComputePrepass");
	if (!insert_position) return nullptr;

	auto data = std::make_unique<ComputePassStep>();
	data->compute_function = compute_function;
	data->debug_name = name;

	ComputePassStep* ptr = data.get();
	compute_prepass_steps[name] = std::move(data);
	ordered_compute_prepass_steps.insert(*insert_position, ptr);
	return ptr;
}

BlitPassStep* PassManager::CreateBlitPass(const BlitPassName& name, TextureAtlas* src, TextureAtlas* dst, const PassAnchor& anchor,
	SDL_GPUFilter filter, SDL_GPULoadOp load_op)
{
	if (!src || !dst) {
		SDL_Log("PassManager::CreateBlitPass: '%s' - src/dst atlas is null.", name.c_str());
		return nullptr;
	}
	auto it_blit = blit_steps.find(name);
	if (it_blit != blit_steps.end()) {
		SDL_Log("PassManager::CreateBlitPass: Blit pass with name '%s' already exists.", name.c_str());
		return it_blit->second.get();
	}
	if (!CanCreatePass(name, "PassManager::CreateBlitPass")) return nullptr;
	const auto insert_position = FrameInsertPosition(anchor, name, "PassManager::CreateBlitPass");
	if (!insert_position) return nullptr;

	src->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
	dst->tci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

	auto data = std::make_unique<BlitPassStep>();
	data->src = src;
	data->dst = dst;
	data->filter = filter;
	data->load_op = load_op;
	data->debug_name = name;

	BlitPassStep* ptr = data.get();
	blit_steps[name] = std::move(data);
	ordered_execution.insert(*insert_position, ptr);
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
	ordered_compute_steps.clear();
	for (const FrameStep& step : ordered_execution) {
		if (RenderPassStep* const* render_step = std::get_if<RenderPassStep*>(&step))
			ordered_passes.push_back(*render_step);
		else if (ComputePassStep* const* compute_step = std::get_if<ComputePassStep*>(&step))
			ordered_compute_steps.push_back(*compute_step);
	}

	for (size_t i = 0; i < ordered_passes.size(); ++i)
		ordered_passes[i]->ordinal = safe_u32(i);

	uint32_t compute_ordinal = 0;
	for (ComputePassStep* cs : ordered_compute_prepass_steps) cs->ordinal = compute_ordinal++;
	for (ComputePassStep* cs : ordered_compute_steps)         cs->ordinal = compute_ordinal++;

	passes_filled = true;
}

std::vector<std::string> PassManager::OrderedPassNames() const
{
	std::vector<std::string> names;
	auto name_of = [](const auto& registry, const auto* step) -> const std::string* {
		for (const auto& [name, owned_step] : registry)
			if (owned_step.get() == step) return &name;
		return nullptr;
	};
	for (const ComputePassStep* prepass : ordered_compute_prepass_steps)
		if (const std::string* name = name_of(compute_prepass_steps, prepass)) names.push_back(*name);
	for (const FrameStep& step : ordered_execution) {
		const std::string* name = std::visit([&](const auto* pass) -> const std::string* {
			using T = std::remove_cv_t<std::remove_pointer_t<decltype(pass)>>;
			if constexpr (std::is_same_v<T, RenderPassStep>)       return name_of(render_steps, pass);
			else if constexpr (std::is_same_v<T, ComputePassStep>) return name_of(compute_steps, pass);
			else                                                   return name_of(blit_steps, pass);
		}, step);
		if (name) names.push_back(*name);
	}
	return names;
}

void PassManager::ExecutePassesSteps(SDL_GPUCommandBuffer* cb, uint8_t pass_frame)
{
	for (const FrameStep& step : ordered_execution) {
		std::visit([&](auto* pass) {
			using T = std::remove_pointer_t<decltype(pass)>;
			if constexpr (std::is_same_v<T, RenderPassStep>)       pass->render_function(cb, this, *pass);
			else if constexpr (std::is_same_v<T, ComputePassStep>) pass->compute_function(cb, this, *pass, pass_frame);
			else if constexpr (std::is_same_v<T, BlitPassStep>)    BlitPassStandardBody(cb, *pass);
		}, step);
	}
}

void PassManager::BlitPassStandardBody(SDL_GPUCommandBuffer* cb, BlitPassStep& bp)
{
	if (!bp.src || !bp.dst) return;

	SDL_GPUTexture* src_tex = bp.src->texture_binding.texture;
	SDL_GPUTexture* dst_tex = bp.dst->texture_binding.texture;
	if (!src_tex || !dst_tex) return;
	if (bp.src->width == 0 || bp.src->height == 0 || bp.dst->width == 0 || bp.dst->height == 0) return;

	SDL_GPUBlitInfo bi{};
	bi.source.texture = src_tex;
	bi.source.mip_level = bp.src_mip;
	bi.source.layer_or_depth_plane = bp.src_layer;
	bi.source.w = bp.src->width;
	bi.source.h = bp.src->height;
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

void PassManager::RenderPassStandardBody(SDL_GPUCommandBuffer* cb, RenderPassStep* render_pass_step, BufferManager* bm, uint32_t region_index, const void* push_data_raw)
{
	const PassRegions& stamped = AskRegions(render_frame);
	uint32_t first_command = 0;
	if (render_pass_step->ordinal < stamped.per_pass.size()) {
		const PassRegion& region = stamped.per_pass[render_pass_step->ordinal];
		if (region_index >= region.command_blocks_count) {
			SDL_Log("RenderPassStandardBody: pass '%s' draws block %u, but its region count instruction asked for %u - the draw reads a neighbour region",
				render_pass_step->debug_name.c_str(), region_index, region.command_blocks_count);
		}
		first_command = region.cmd_base + region_index * region.commands;
	}
	const uint32_t additional_offset = first_command * safe_u32(sizeof(SDL_GPUIndexedIndirectDrawCommand));

	auto& tex_data = render_pass_step->renderPassTexsData;

	SDL_GPUColorTargetInfo color_infos[MAX_COLOR_TARGETS];
	const uint32_t color_count = tex_data.CollectColorTargetInfos(color_infos, MAX_COLOR_TARGETS);

	SDL_GPURenderPass* rp = nullptr;
	rp = SDL_BeginGPURenderPass(cb, color_infos, color_count, &tex_data.depthTargetInfo);
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
	const RenderSnap::ComputeLayout* layout = compute_layouts[pass_frame];
	if (!layout || compute_step->ordinal >= layout->passes.size()) return;

	for (const RenderSnap::ComputeDispatch& dispatch : layout->passes[compute_step->ordinal]) {
		glm::uvec3 elements{ 1, 1, 1 };
		if (dispatch.dispatch_func) {
			DispatchSizeBinder dispatch_binder{};
			dispatch_binder.frame = pass_frame;
			dispatch.dispatch_func(dispatch_binder, dispatch_data_raw);
			elements = dispatch_binder.element_count;
		}

		if (elements.x == 0 || elements.y == 0 || elements.z == 0) continue;

		{
			const PushInput push_in{ push_data_raw, nullptr };
			for (const PushInstruction& pi : dispatch.push_instructions)
				pi.fn(PushConstantBinder{ cb, pi.stage, pi.uniform_slot, pass_frame }, push_in);
		}

		std::vector<SDL_GPUStorageBufferReadWriteBinding> storage_buffer_bindings =
			bm->BuildBindGPUComputeRWBuffers(dispatch.rw_storage_buffers, pass_frame);

		std::vector<SDL_GPUStorageTextureReadWriteBinding> rw_textures;
		rw_textures.reserve(dispatch.rw_storage_textures.size());
		for (const auto& r : dispatch.rw_storage_textures)
			rw_textures.push_back({ r.atlas->texture_binding.texture, r.mip_level, r.layer, false });

		SDL_GPUComputePass* cmp = SDL_BeginGPUComputePass(cb,
			rw_textures.data(), safe_u32(rw_textures.size()),
			storage_buffer_bindings.data(), safe_u32(storage_buffer_bindings.size()));

		SDL_BindGPUComputePipeline(cmp, dispatch.pipeline.get());
		if (!dispatch.texture_binding.empty()) {
			std::vector<SDL_GPUTextureSamplerBinding> samplers;
			samplers.reserve(dispatch.texture_binding.size());
			for (TextureAtlas* a : dispatch.texture_binding)
				samplers.push_back(a->texture_binding);
			SDL_BindGPUComputeSamplers(cmp, 0, samplers.data(), safe_u32(samplers.size()));
		}
		if (!dispatch.ro_storage_textures.empty()) {
			std::vector<SDL_GPUTexture*> ro_textures;
			ro_textures.reserve(dispatch.ro_storage_textures.size());
			for (TextureAtlas* a : dispatch.ro_storage_textures)
				ro_textures.push_back(a->texture_binding.texture);
			SDL_BindGPUComputeStorageTextures(cmp, 0, ro_textures.data(), safe_u32(ro_textures.size()));
		}
		if (!dispatch.ro_storage_buffers.empty()) {
			bm->BindGPUComputeRO_Buffers(cmp, 0, dispatch.ro_storage_buffers, pass_frame);
		}

		const uint32_t gx = (elements.x + dispatch.threadcount_x - 1) / dispatch.threadcount_x;
		const uint32_t gy = (elements.y + dispatch.threadcount_y - 1) / dispatch.threadcount_y;
		const uint32_t gz = (elements.z + dispatch.threadcount_z - 1) / dispatch.threadcount_z;
		SDL_DispatchGPUCompute(cmp, gx, gy, gz);

		SDL_EndGPUComputePass(cmp);
	}
}

void PassManager::CreateRegionCountInstruction(const RenderPassName& name, std::function<uint32_t(uint8_t)> fn)
{
	region_count_instructions[name] = std::move(fn);
}

void PassManager::StampRegions(uint8_t slot, const RenderSnap::BatchLayout* layout)
{
	PassRegions& stamped = regions[slot];
	stamped.per_pass.clear();
	stamped.total_commands = 0;
	stamped.total_pib = 0;
	if (!layout) return;

	stamped.per_pass.resize(layout->passes.size());

	for (uint32_t i = 0; i < stamped.per_pass.size(); ++i) {
		const RenderSnap::PassDrawList& pass_list = layout->passes[i];
		PassRegion& region = stamped.per_pass[i];
		region.command_blocks_count = 1;
		region.commands = pass_list.num_commands;
		region.pib = pass_list.num_instances;
		region.first_pib = pass_list.first_instance;
	}

	for (const auto& [name, count_fn] : region_count_instructions) {
		if (!count_fn) continue;
		RenderPassStep* rp = GetRenderPassStep(name);
		if (!rp || rp->ordinal >= stamped.per_pass.size()) continue;
		stamped.per_pass[rp->ordinal].command_blocks_count = count_fn(slot);
	}

	uint32_t cmd_base = 0;
	uint32_t pib_base = 0;
	for (PassRegion& region : stamped.per_pass) {
		region.cmd_base = cmd_base;
		region.pib_base = pib_base;
		cmd_base += region.command_blocks_count * region.commands;
		pib_base += region.command_blocks_count * region.pib;
	}

	stamped.total_commands = cmd_base;
	stamped.total_pib = pib_base;
}

RenderPassStep* PassManager::GetRenderPassStep(const RenderPassName& name)
{
	auto it = render_steps.find(name);
	if (it != render_steps.end()) {
		return it->second.get();
	}
	SDL_Log("PassManager::Render pass '%s' not found", name.c_str());
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

inline void PassManager::ExecuteRenderBatches(SDL_GPUCommandBuffer* cb, SDL_GPURenderPass* rp, const RenderPassStep& render_pass_step, BufferManager* bm, uint32_t additional_offset, const void* push_data_raw)
{
	if (!render_layout || render_pass_step.ordinal >= render_layout->passes.size()) return;
	const RenderSnap::PassDrawList& pass_list = render_layout->passes[render_pass_step.ordinal];

	SDL_GPUBuffer* indirect_buf = bm->_GetGPUBufferForFrame(render_layout->indirectBuffer, render_frame);
	if (!indirect_buf) {
		SDL_Log("ExecuteRenderBatches: indirect buffer is missing - pass draw list skipped");
		return;
	}

	const std::vector<SDL_GPUTextureSamplerBinding>& global_samplers = pass_list.global_texture_bindings;
	const uint32_t global_sampler_count = safe_u32(global_samplers.size());

	int draw_calls = 0;
	for (const RenderSnap::ShaderGroup& shader_batch : pass_list.shaders)
	{
		SDL_BindGPUGraphicsPipeline(rp, shader_batch.pipeline.get());
		SDL_BindGPUFragmentSamplers(rp, 0, global_samplers.data(), global_sampler_count);

		if (!bm->BindGPUVertexBuffers(rp, shader_batch.vertexBuffers)) {
			SDL_Log("ExecuteRenderBatches: vertex stream bind failed - shader batch skipped");
			continue;
		}
		if (!bm->BindGPUIndexBuffer(rp, shader_batch.indexBuffer, 0)) {
			SDL_Log("ExecuteRenderBatches: index buffer bind failed - shader batch skipped");
			continue;
		}

		if (!shader_batch.vertexStorageBuffers.empty()) {
			bm->BindGPUVertexStorageBuffers(rp, 0, shader_batch.vertexStorageBuffers, render_frame);
		}
		if (!shader_batch.fragmentStorageBuffers.empty()) {
			bm->BindGPUFragmentStorageBuffers(rp, 0, shader_batch.fragmentStorageBuffers, render_frame);
		}

		for (const RenderSnap::AtlasGroup& atlas_batch : shader_batch.atlases) {
			if (!atlas_batch.texture_binding.empty()) {
				SDL_BindGPUFragmentSamplers(rp, global_sampler_count, atlas_batch.texture_binding.data(), safe_u32(atlas_batch.texture_binding.size()));
			}
			for (const RenderSnap::TextureDraw& texture_batch : atlas_batch.draws) {
				const PushInput push_in{ push_data_raw, &texture_batch };
				for (const PushInstruction& pi : shader_batch.push_instructions)
					pi.fn(PushConstantBinder{ cb, pi.stage, pi.uniform_slot, render_frame }, push_in);

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
