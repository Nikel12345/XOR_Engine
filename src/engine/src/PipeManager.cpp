#include "PCH.h"
#include "PipeManager.h"
#include "RenderCommandData.h"
#include "PassManager.h"
#include "ShaderManager.h"


size_t GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& k) const
{
    uint64_t h = 0;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(std::hash<VertexShaderId>{}(k.vs_id));
    mix(reinterpret_cast<uintptr_t>(k.vsd));
    mix(std::hash<FragmentShaderId>{}(k.fs_id));
    mix(reinterpret_cast<uintptr_t>(k.fsd));
    mix(k.spd.cull_mode);
    mix(k.spd.fill_mode);
    mix(k.spd.primitive_type);
    mix(k.spd.depth_compare_op);
    mix((uint64_t{ k.spd.depth_test } << 0) | (uint64_t{ k.spd.depth_write } << 1)
      | (uint64_t{ k.spd.stencil_test } << 2) | (uint64_t{ k.spd.color_blend } << 3)
      | (uint64_t{ k.spd.rasterizer_bias.enable_depth_bias } << 4));
    for (SDL_GPUTextureFormat f : k.color_formats) mix(f);
    mix(k.depth_format);
    return h;
}

PipeManager::PipeManager(SDL_GPUDevice* dev, SDL_Window* win) {
    this->win = win;
    this->dev = dev;
}

// Форматы таргетов прохода входят в ключ: перенастройка прохода без взвода флага пересборки
// пайплайнов оставит его программы без пайплайна.
void PipeManager::CreateGraphicsPiplenes(const ShaderProgramRegistry& shader_programs, ShaderManager* sm, PassManager* pass_manager)
{
    decltype(graphics_pipelines) live;
    for (int32_t i = 0; i < shader_programs.Count(); ++i) {
        const ShaderProgramCell& cell = shader_programs.At(i);
        if (!cell.object) continue;

        std::optional<GraphicsPipelineKey> key = MakeGraphicsPipelineKey(*cell.object, sm, pass_manager);
        if (!key) {
            SDL_Log("Pipeline for shader program '%s': vertex/fragment shader or render pass '%s' not found",
                cell.name.c_str(), cell.object->render_pass_name.c_str());
            continue;
        }
        if (live.count(*key)) continue;

        auto built = graphics_pipelines.find(*key);
        std::shared_ptr<SDL_GPUGraphicsPipeline> pipe =
            built != graphics_pipelines.end() ? built->second : CreateGraphicsPipeline(*key);
        if (!pipe) {
            SDL_Log("Failed to create pipeline for shader program: %s", cell.name.c_str());
            continue;
        }
        live.emplace(std::move(*key), std::move(pipe));
    }
    graphics_pipelines = std::move(live);
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

std::optional<GraphicsPipelineKey> PipeManager::MakeGraphicsPipelineKey(const ShaderProgram& sp, ShaderManager* sm, PassManager* pass_manager) const
{
    GraphicsPipelineKey key;
    key.vs_id = sp.vs_id;
    key.vsd   = sm->GetVertexShader(sp.vs_id);
    key.fs_id = sp.fs_id;
    key.fsd   = sm->GetFragmentShader(sp.fs_id);
    const RenderPassStep* pass = pass_manager->FindRenderPassStep(sp.render_pass_name);
    if (!key.vsd || !key.fsd || !pass) return std::nullopt;
    if (!key.vsd->shader_data.shader || !key.fsd->shader_data.shader) return std::nullopt;

    key.spd = sp.spd;
    key.color_formats.reserve(pass->renderPassTexsData.color_targets.size());
    for (const ColorTarget& target : pass->renderPassTexsData.color_targets)
        key.color_formats.push_back(target.format != SDL_GPU_TEXTUREFORMAT_INVALID
            ? target.format : SDL_GetGPUSwapchainTextureFormat(dev, win));
    key.depth_format = pass->renderPassTexsData.depth_format;
    return key;
}

std::shared_ptr<SDL_GPUGraphicsPipeline> PipeManager::FindGraphicsPipeline(const ShaderProgram& sp, ShaderManager* sm, PassManager* pass_manager) const
{
    std::optional<GraphicsPipelineKey> key = MakeGraphicsPipelineKey(sp, sm, pass_manager);
    if (!key) return {};
    auto it = graphics_pipelines.find(*key);
    return it != graphics_pipelines.end() ? it->second : nullptr;
}

std::shared_ptr<SDL_GPUGraphicsPipeline> PipeManager::CreateGraphicsPipeline(const GraphicsPipelineKey& key)
{
    const ShaderProgramDescription& spd = key.spd;

    SDL_GPUGraphicsPipelineCreateInfo pci;
    SDL_zero(pci);

    pci.vertex_shader = key.vsd->shader_data.shader.get();
    pci.fragment_shader = key.fsd->shader_data.shader.get();

    pci.primitive_type = spd.primitive_type;
    pci.rasterizer_state.fill_mode = spd.fill_mode;
    pci.rasterizer_state.cull_mode = spd.cull_mode;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    pci.rasterizer_state.enable_depth_bias = spd.rasterizer_bias.enable_depth_bias;
    pci.rasterizer_state.depth_bias_constant_factor = spd.rasterizer_bias.depth_bias_constant_factor;
    pci.rasterizer_state.depth_bias_slope_factor = spd.rasterizer_bias.depth_bias_slope_factor;
    pci.rasterizer_state.depth_bias_clamp = spd.rasterizer_bias.depth_bias_clamp;

    pci.vertex_input_state.num_vertex_buffers = safe_u32(key.vsd->vbs.size());
    pci.vertex_input_state.vertex_buffer_descriptions = key.vsd->vbs.data();
    pci.vertex_input_state.num_vertex_attributes = safe_u32(key.vsd->attributes.size());
    pci.vertex_input_state.vertex_attributes = key.vsd->attributes.data();

    pci.depth_stencil_state.enable_depth_test = spd.depth_test;
    pci.depth_stencil_state.enable_depth_write = spd.depth_write;
    pci.depth_stencil_state.enable_stencil_test = spd.stencil_test;
    pci.depth_stencil_state.compare_op = spd.depth_compare_op;

    std::vector<SDL_GPUColorTargetDescription> ctds;
    ctds.reserve(key.color_formats.size());
    for (SDL_GPUTextureFormat fmt : key.color_formats) {
        SDL_GPUColorTargetDescription ctd;
        SDL_zero(ctd);
        ctd.format = fmt;
        ctd.blend_state.enable_blend = spd.color_blend;
        if (spd.color_blend) {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        ctds.push_back(ctd);
    }
    pci.target_info.num_color_targets = safe_u32(ctds.size());
    pci.target_info.color_target_descriptions = ctds.empty() ? nullptr : ctds.data();

    pci.target_info.has_depth_stencil_target = (key.depth_format != SDL_GPU_TEXTUREFORMAT_INVALID);
    pci.target_info.depth_stencil_format = key.depth_format;

    SDL_GPUGraphicsPipeline* pipe = SDL_CreateGPUGraphicsPipeline(dev, &pci);
    if (!pipe) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Pipeline creation failed: %s",
            SDL_GetError()
        );
        return {};
    }

    SDL_GPUDevice* device = dev;
    return std::shared_ptr<SDL_GPUGraphicsPipeline>(pipe, [device](SDL_GPUGraphicsPipeline* p) {
        SDL_ReleaseGPUGraphicsPipeline(device, p);
    });
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

SDL_GPUColorTargetDescription PipeManager::MakeNoColorTarget() {
	SDL_GPUColorTargetDescription ctd{};
	SDL_zero(ctd);
	return ctd;
}

std::shared_ptr<SDL_GPUComputePipeline> PipeManager::GetComputePipeline(ComputeShaderProgram* sp)
{
    if (!sp->pipeline)
        SDL_Log("PipeManager::Compute pipeline not built for program '%s'!", sp->debug_name.c_str());
    return sp->pipeline;
}



