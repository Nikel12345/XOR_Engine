#include "PCH.h"
#include "PipeManager.h"
#include "RenderCommandData.h"
#include "PassManager.h"
#include "ShaderManager.h"


PipeManager::PipeManager(SDL_GPUDevice* dev, SDL_Window* win) {
    this->win = win;
    this->dev = dev;
}

void PipeManager::CreateGraphicsPiplenes(ShaderProgramRegistry& shader_programs, ShaderManager* sm, PassManager* pass_manager)
{
    for (int32_t i = 0; i < shader_programs.Count(); ++i) {
        const ShaderProgramCell& cell = shader_programs.At(i);
        if (!cell.object) continue;
        auto pipe = GetOrCreatePipeline(cell.object.get(), sm, pass_manager);
        if (!pipe) {
			SDL_Log("Failed to create pipeline for shader program: %s", cell.name.c_str());
        }
	}
}

void PipeManager::CreateComputePipelines(ComputeProgramRegistry& compute_shader_programs, ShaderManager* sm)
{
    for (int32_t i = 0; i < compute_shader_programs.Count(); ++i) {
        const ComputeProgramCell& cell = compute_shader_programs.At(i);
        if (!cell.object) continue;
		auto pipe = GetOrCreateComputePipeline(cell.object.get(), sm);
        if (!pipe) {
            SDL_Log("Failed to create compute pipeline for shader program: %s", cell.name.c_str());
		}
    }
}

std::shared_ptr<SDL_GPUGraphicsPipeline> PipeManager::GetOrCreatePipeline(ShaderProgram* sp, ShaderManager* sm, PassManager* pass_manager)
{
    if (sp->pipeline) return sp->pipeline;

    RenderPassStep* pass = pass_manager ? pass_manager->GetRenderPassStep(sp->render_pass_name) : nullptr;
    if (!pass) {
        SDL_Log("Pipeline '%s': render pass '%s' not found", sp->debug_name.c_str(), sp->render_pass_name.c_str());
        return {};
    }

    // vs/fs — из реестра ShaderManager (sp хранит только ссылки).
    VertexShaderData*   vsd = sm->GetVertexShader(sp->vs_id);
    FragmentShaderData* fsd = sm->GetFragmentShader(sp->fs_id);
    if (!vsd) SDL_Log("Pipeline '%s': vertex shader '%s' not found in registry", sp->debug_name.c_str(), sm->VertexShaders().NameOf(sp->vs_id).c_str());
    if (!fsd) SDL_Log("Pipeline '%s': fragment shader '%s' not found in registry", sp->debug_name.c_str(), sm->FragmentShaders().NameOf(sp->fs_id).c_str());
    if (!vsd || !fsd) return {};

    SDL_GPUGraphicsPipelineCreateInfo pci;
    SDL_zero(pci);

    pci.vertex_shader = vsd->shader_data.shader.get();
    pci.fragment_shader = fsd->shader_data.shader.get();

    pci.primitive_type = sp->spd.primitive_type; 
    pci.rasterizer_state.fill_mode = sp->spd.fill_mode;
    pci.rasterizer_state.cull_mode = sp->spd.cull_mode;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    pci.rasterizer_state.enable_depth_bias = sp->spd.rasterizer_bias.enable_depth_bias;
    pci.rasterizer_state.depth_bias_constant_factor = sp->spd.rasterizer_bias.depth_bias_constant_factor;
    pci.rasterizer_state.depth_bias_slope_factor = sp->spd.rasterizer_bias.depth_bias_slope_factor;
    pci.rasterizer_state.depth_bias_clamp = sp->spd.rasterizer_bias.depth_bias_clamp;

    pci.vertex_input_state.num_vertex_buffers = safe_u32(vsd->vbs.size());
    pci.vertex_input_state.vertex_buffer_descriptions = vsd->vbs.data();
    pci.vertex_input_state.num_vertex_attributes = safe_u32(vsd->attributes.size());
    pci.vertex_input_state.vertex_attributes = vsd->attributes.data();

    pci.depth_stencil_state.enable_depth_test = sp->spd.depth_test;
    pci.depth_stencil_state.enable_depth_write = sp->spd.depth_write;
    pci.depth_stencil_state.enable_stencil_test = sp->spd.stencil_test;
    pci.depth_stencil_state.compare_op = sp->spd.depth_compare_op;


    const auto& color_targets = pass->renderPassTexsData.color_targets;
    std::vector<SDL_GPUColorTargetDescription> ctds;
    ctds.reserve(color_targets.size());
    for (const ColorTarget& target : color_targets) {
        const SDL_GPUTextureFormat fmt = target.format;
        SDL_GPUColorTargetDescription ctd;
        if (fmt != SDL_GPU_TEXTUREFORMAT_INVALID) {
            SDL_zero(ctd);
            ctd.format = fmt;
        }
        else {
            ctd = MakeDefaultColorTarget();
        }
        ctd.blend_state.enable_blend = sp->spd.color_blend;
        if (sp->spd.color_blend) {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        ctds.push_back(ctd);
    }
    if (!ctds.empty()) {
        pci.target_info.num_color_targets = safe_u32(ctds.size());
        pci.target_info.color_target_descriptions = ctds.data();
    }
    else {
        pci.target_info.num_color_targets = 0;
        pci.target_info.color_target_descriptions = nullptr;
    }

    // Наличие depth-таргета ВЫВОДИТСЯ из формата, а не считается данностью.
    const SDL_GPUTextureFormat ds_fmt = pass->renderPassTexsData.depth_format;
    pci.target_info.has_depth_stencil_target = (ds_fmt != SDL_GPU_TEXTUREFORMAT_INVALID);
    pci.target_info.depth_stencil_format = ds_fmt;

	SDL_GPUGraphicsPipeline* pipe = nullptr;
    pipe = SDL_CreateGPUGraphicsPipeline(dev, &pci);

    if (!pipe) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Pipeline creation failed: %s",
            SDL_GetError()
        );
        return {};
    }

    SDL_GPUDevice* device = dev;
    sp->pipeline = std::shared_ptr<SDL_GPUGraphicsPipeline>(pipe, [device](SDL_GPUGraphicsPipeline* p) {
        SDL_ReleaseGPUGraphicsPipeline(device, p);
    });
    return sp->pipeline;
}

std::shared_ptr<SDL_GPUComputePipeline> PipeManager::GetOrCreateComputePipeline(ComputeShaderProgram* sp, ShaderManager* sm)
{
    if (sp->pipeline) return sp->pipeline;

    ComputeShaderData* csd = sm->GetComputeShader(sp->cs_id);   // cs из реестра
    if (!csd) {
        SDL_Log("Compute pipeline '%s': cs '%s' not found in registry", sp->debug_name.c_str(), sm->ComputeShaders().NameOf(sp->cs_id).c_str());
        return {};
    }

    SDL_GPUComputePipelineCreateInfo ci;
    SDL_zero(ci);
    ci.code = csd->spv_code;
	ci.code_size = csd->spv_size;
    ci.entrypoint = "main";

    const SDL_GPUShaderFormat fmt = SDL_GetGPUShaderFormats(dev);
    if (fmt & SDL_GPU_SHADERFORMAT_SPIRV) ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    else if (fmt & SDL_GPU_SHADERFORMAT_DXIL)  ci.format = SDL_GPU_SHADERFORMAT_DXIL;
    else if (fmt & SDL_GPU_SHADERFORMAT_MSL)   ci.format = SDL_GPU_SHADERFORMAT_MSL;

    ci.num_samplers = csd->num_samplers;
    ci.num_readonly_storage_textures = csd->num_readonly_storage_textures;
    ci.num_readonly_storage_buffers = csd->num_readonly_storage_buffers;
    ci.num_readwrite_storage_textures = csd->num_readwrite_storage_textures;
    ci.num_readwrite_storage_buffers = csd->num_readwrite_storage_buffers;
    ci.num_uniform_buffers = csd->num_uniform_buffers;
    ci.threadcount_x = csd->threadcount_x;
    ci.threadcount_y = csd->threadcount_y;
    ci.threadcount_z = csd->threadcount_z;

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(dev, &ci);
    if (!pipeline) {
        SDL_Log("Failed to create compute pipeline '%s': %s", sp->debug_name.c_str(), SDL_GetError());
        return {};
    }

    SDL_GPUDevice* device = dev;
    sp->pipeline = std::shared_ptr<SDL_GPUComputePipeline>(pipeline, [device](SDL_GPUComputePipeline* p) {
        SDL_ReleaseGPUComputePipeline(device, p);
    });
    return sp->pipeline;
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

std::shared_ptr<SDL_GPUGraphicsPipeline> PipeManager::GetGraphicPipeline(ShaderProgram* sp)
{
    if (!sp->pipeline)
        SDL_Log("PipeManager::Graphic pipeline not built for shader program '%s'!", sp->debug_name.c_str());
    return sp->pipeline;
}

std::shared_ptr<SDL_GPUComputePipeline> PipeManager::GetComputePipeline(ComputeShaderProgram* sp)
{
    if (!sp->pipeline)
        SDL_Log("PipeManager::Compute pipeline not built for program '%s'!", sp->debug_name.c_str());
    return sp->pipeline;
}



