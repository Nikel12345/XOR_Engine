#include "PCH.h"
#include "PipeManager.h"
#include "RenderCommandData.h"


PipeManager::PipeManager(SDL_GPUDevice* dev, SDL_Window* win) {
    this->win = win;
    this->dev = dev;
}

void PipeManager::CreateGraphicsPiplenes(std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& shader_programs)
{
    for (auto& pair : shader_programs) {
        ShaderProgram* sp = pair.second.get();
        SDL_GPUGraphicsPipeline* pipe = GetOrCreatePipeline(sp);
        if (!pipe) {
			SDL_Log("Failed to create pipeline for shader program: %s", pair.first.c_str());
        }
	}
}

void PipeManager::CreateComputePipelines(std::vector<std::unique_ptr<ComputeShaderProgram>>& compute_shader_programs)
{
    for (auto& pair : compute_shader_programs) {
        ComputeShaderProgram* sp = pair.get();
		SDL_GPUComputePipeline* pipe = GetOrCreateComputePipeline(sp);
        if (!pipe) {
            SDL_Log("Failed to create compute pipeline for shader program: %s", sp->debug_name.c_str());
		}
    }
}

SDL_GPUGraphicsPipeline* PipeManager::GetOrCreatePipeline(ShaderProgram* sp)
{
    auto it = graphics_pipelines.find(sp);
    if (it != graphics_pipelines.end()) {
		if (it->second == nullptr) {
			SDL_Log("Pipeline for given ShaderProgram is nullptr!");
        }
        return it->second;
    }

    SDL_GPUGraphicsPipelineCreateInfo pci;
    SDL_zero(pci);

    pci.vertex_shader = sp->vs.shader_data.shader.get();
    pci.fragment_shader = sp->fs.shader_data.shader.get();

    pci.primitive_type = sp->spd->primitive_type; 
    pci.rasterizer_state.fill_mode = sp->spd->fill_mode;
    pci.rasterizer_state.cull_mode = sp->spd->cull_mode;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    pci.rasterizer_state.enable_depth_bias = sp->spd->rasterizer_bias.enable_depth_bias;
    pci.rasterizer_state.depth_bias_constant_factor = sp->spd->rasterizer_bias.depth_bias_constant_factor;
    pci.rasterizer_state.depth_bias_slope_factor = sp->spd->rasterizer_bias.depth_bias_slope_factor;
    pci.rasterizer_state.depth_bias_clamp = sp->spd->rasterizer_bias.depth_bias_clamp;

    pci.vertex_input_state.num_vertex_buffers = safe_u32(sp->vs.vbs.size());
    pci.vertex_input_state.vertex_buffer_descriptions = sp->vs.vbs.data();
    pci.vertex_input_state.num_vertex_attributes = safe_u32(sp->vs.attributes.size());
    pci.vertex_input_state.vertex_attributes = sp->vs.attributes.data();

    pci.depth_stencil_state.enable_depth_test = sp->spd->depth_test;
    pci.depth_stencil_state.enable_depth_write = sp->spd->depth_write;
    pci.depth_stencil_state.enable_stencil_test = sp->spd->stencil_test;
    pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;


    SDL_GPUColorTargetDescription ctd;
    if(sp->associated_render_pass->renderPassTexsData.numColorTargets > 0) {
        SDL_zero(ctd);
        if (sp->associated_render_pass->renderPassTexsData.color_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
            ctd.format = sp->associated_render_pass->renderPassTexsData.color_format;
        }
        else {
            ctd = MakeDefaultColorTarget();
        }
        ctd.blend_state.enable_blend = sp->spd->color_blend;
        if (sp->spd->color_blend) {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        pci.target_info.num_color_targets = 1;
        pci.target_info.color_target_descriptions = &ctd;
    }
    else {
        pci.target_info.num_color_targets = 0;
        pci.target_info.color_target_descriptions = nullptr;
    }

    pci.target_info.has_depth_stencil_target = true;
    pci.target_info.depth_stencil_format = sp->associated_render_pass->renderPassTexsData.depth_format;

	SDL_GPUGraphicsPipeline* pipe = nullptr;
    pipe = SDL_CreateGPUGraphicsPipeline(dev, &pci);

    if (!pipe) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Pipeline creation failed: %s",
            SDL_GetError()
        );
        return nullptr;
    }

    graphics_pipelines.emplace(sp, pipe);
    return pipe;
}

SDL_GPUComputePipeline* PipeManager::GetOrCreateComputePipeline(ComputeShaderProgram* sp)
{
    auto it = compute_pipelines.find(sp);
    if (it != compute_pipelines.end()) {
        if (it->second == nullptr)
            SDL_Log("Pipeline for given ComputeShaderProgram is nullptr!");
        return it->second;
    }

    SDL_GPUComputePipelineCreateInfo ci;
    SDL_zero(ci);
    ci.code = sp->cs.spv_code;
	ci.code_size = sp->cs.spv_size;
    ci.entrypoint = "main";

    const SDL_GPUShaderFormat fmt = SDL_GetGPUShaderFormats(dev);
    if (fmt & SDL_GPU_SHADERFORMAT_SPIRV) ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    else if (fmt & SDL_GPU_SHADERFORMAT_DXIL)  ci.format = SDL_GPU_SHADERFORMAT_DXIL;
    else if (fmt & SDL_GPU_SHADERFORMAT_MSL)   ci.format = SDL_GPU_SHADERFORMAT_MSL;

    ci.num_samplers = sp->cs.num_samplers;
    ci.num_readonly_storage_textures = sp->cs.num_readonly_storage_textures;
    ci.num_readonly_storage_buffers = sp->cs.num_readonly_storage_buffers;
    ci.num_readwrite_storage_textures = sp->cs.num_readwrite_storage_textures;
    ci.num_readwrite_storage_buffers = sp->cs.num_readwrite_storage_buffers;
    ci.num_uniform_buffers = sp->cs.num_uniform_buffers;
    ci.threadcount_x = sp->cs.threadcount_x;
    ci.threadcount_y = sp->cs.threadcount_y;
    ci.threadcount_z = sp->cs.threadcount_z;

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(dev, &ci);
    if (!pipeline)
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());

    compute_pipelines.emplace(sp, pipeline);
    //SDL_free(sp->cs.spv_code);
    return pipeline;
}

SDL_GPUColorTargetDescription PipeManager::MakeDefaultColorTarget()
{
    SDL_GPUColorTargetDescription ctd{};
    SDL_zero(ctd);
    ctd.blend_state.enable_blend = true;
    ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ctd.format = SDL_GetGPUSwapchainTextureFormat(dev, win);
    return ctd;
}

SDL_GPUColorTargetDescription PipeManager::MakeNoColorTarget() {
	SDL_GPUColorTargetDescription ctd{};
	SDL_zero(ctd);
	return ctd;
}

SDL_GPUGraphicsPipeline* PipeManager::GetGraphicPipeline(ShaderProgram* sp)
{
    auto it = graphics_pipelines.find(sp);
    if (it != graphics_pipelines.end()) {
        if (it->second == nullptr) {
            SDL_Log("Pipeline for given ShaderProgram is nullptr!");
        }
        return it->second;
	}
	SDL_Log("PipeManager::Graphic pipeline not found for given ShaderProgram!");
	return nullptr;
}

SDL_GPUComputePipeline* PipeManager::GetComputePipeline(ComputeShaderProgram* sp)
{
    auto it = compute_pipelines.find(sp);
    if (it != compute_pipelines.end()) {
        if (it->second == nullptr) {
            SDL_Log("Pipeline for given ComputeShaderProgram is nullptr!");
			assert(it->second && "Compute pipeline should not be null here!");
		}
        return it->second;
    }
    SDL_Log("PipeManager::Compute pipeline not found for given ComputeShaderProgram!");
	return nullptr;
}

PipeManager::~PipeManager()
{
    for (auto& pair : graphics_pipelines) {
        if (pair.second) {
            SDL_ReleaseGPUGraphicsPipeline(dev, pair.second);
        }
        graphics_pipelines.clear();
    }
}

