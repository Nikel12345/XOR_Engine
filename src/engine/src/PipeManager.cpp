#include "PCH.h"
#include "PipeManager.h"
#include "RenderCommandData.h"
#include "ShaderManager.h"   // резолв vs/fs/cs по имени (sp/csp хранят имена)


PipeManager::PipeManager(SDL_GPUDevice* dev, SDL_Window* win) {
    this->win = win;
    this->dev = dev;
}

void PipeManager::TrashPipelines(uint64_t fences_done, uint64_t graphics_required_epoch, uint64_t compute_rebuild_epoch)
{
    // Дренаж на sim (пуш — команды, дренаж — prepare: один поток, без замков).
    // Графика: армирование = планка эпох слотов ушла дальше эпохи инвалидации (рендер больше
    // не покажет слоты со слепками, держащими этот указатель); затем стамп fences_done +
    // BUFFERING_LEVEL добивает in-flight (fence одного queue сигналят в порядке сабмита).
    // До армирования запись живёт сколько угодно — страховка от «sim не успел пере-prepare,
    // а счётчик уже дотикал».
    auto it = pipeline_trash.begin();
    while (it != pipeline_trash.end()) {
        if (graphics_required_epoch <= it->epoch) { ++it; continue; }                 // не армирован
        if (it->ready_at == 0) { it->ready_at = fences_done + BUFFERING_LEVEL; ++it; continue; }
        if (fences_done >= it->ready_at) {
            SDL_ReleaseGPUGraphicsPipeline(dev, it->pipe);
            it = pipeline_trash.erase(it);
        }
        else ++it;
    }

    // Compute: гейт — эпоха пересборок compute-дерева (после неё дерево держит новый указатель).
    auto ct = compute_trash.begin();
    while (ct != compute_trash.end()) {
        if (compute_rebuild_epoch <= ct->epoch) { ++ct; continue; }
        if (ct->ready_at == 0) { ct->ready_at = fences_done + BUFFERING_LEVEL; ++ct; continue; }
        if (fences_done >= ct->ready_at) {
            SDL_ReleaseGPUComputePipeline(dev, ct->pipe);
            ct = compute_trash.erase(ct);
        }
        else ++ct;
    }
}

void PipeManager::CreateGraphicsPiplenes(std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& shader_programs, ShaderManager* sm)
{
    for (auto& pair : shader_programs) {
        ShaderProgram* sp = pair.second.get();
        SDL_GPUGraphicsPipeline* pipe = GetOrCreatePipeline(sp, sm);
        if (!pipe) {
			SDL_Log("Failed to create pipeline for shader program: %s", pair.first.c_str());
        }
	}
}

void PipeManager::CreateComputePipelines(std::vector<std::unique_ptr<ComputeShaderProgram>>& compute_shader_programs, ShaderManager* sm)
{
    for (auto& pair : compute_shader_programs) {
        ComputeShaderProgram* sp = pair.get();
		SDL_GPUComputePipeline* pipe = GetOrCreateComputePipeline(sp, sm);
        if (!pipe) {
            SDL_Log("Failed to create compute pipeline for shader program: %s", sp->debug_name.c_str());
		}
    }
}

SDL_GPUGraphicsPipeline* PipeManager::GetOrCreatePipeline(ShaderProgram* sp, ShaderManager* sm)
{
    auto it = graphics_pipelines.find(sp);
    if (it != graphics_pipelines.end()) {
		if (it->second == nullptr) {
			SDL_Log("Pipeline for given ShaderProgram is nullptr!");
        }
        return it->second;
    }

    // vs/fs — по имени из реестра ShaderManager (sp хранит только имена).
    VertexShaderData*   vsd = sm->GetVertexShader(sp->vs_name);
    FragmentShaderData* fsd = sm->GetFragmentShader(sp->fs_name);
    if (!vsd || !fsd) {
        SDL_Log("Pipeline: vs '%s' or fs '%s' not found in registry", sp->vs_name.c_str(), sp->fs_name.c_str());
        return nullptr;
    }

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


    const auto& color_formats = sp->associated_render_pass->renderPassTexsData.color_formats;
    std::vector<SDL_GPUColorTargetDescription> ctds;
    ctds.reserve(color_formats.size());
    for (SDL_GPUTextureFormat fmt : color_formats) {
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

SDL_GPUComputePipeline* PipeManager::GetOrCreateComputePipeline(ComputeShaderProgram* sp, ShaderManager* sm)
{
    auto it = compute_pipelines.find(sp);
    if (it != compute_pipelines.end()) {
        if (it->second == nullptr)
            SDL_Log("Pipeline for given ComputeShaderProgram is nullptr!");
        return it->second;
    }

    ComputeShaderData* csd = sm->GetComputeShader(sp->cs_name);   // cs по имени из реестра
    if (!csd) {
        SDL_Log("Compute pipeline: cs '%s' not found in registry", sp->cs_name.c_str());
        return nullptr;
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
    if (!pipeline)
        SDL_Log("Failed to create compute pipeline: %s", SDL_GetError());

    compute_pipelines.emplace(sp, pipeline);
    //SDL_free(csd->spv_code);
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
    for (auto& pending : pipeline_trash)   // дочищаем отложенные
        if (pending.pipe) SDL_ReleaseGPUGraphicsPipeline(dev, pending.pipe);
    pipeline_trash.clear();
    for (auto& pending : compute_trash)
        if (pending.pipe) SDL_ReleaseGPUComputePipeline(dev, pending.pipe);
    compute_trash.clear();

    for (auto& pair : graphics_pipelines)
        if (pair.second) SDL_ReleaseGPUGraphicsPipeline(dev, pair.second);
    graphics_pipelines.clear();

    for (auto& pair : compute_pipelines)
        if (pair.second) SDL_ReleaseGPUComputePipeline(dev, pair.second);
    compute_pipelines.clear();
}

