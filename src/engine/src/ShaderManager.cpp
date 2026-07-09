#include "PCH.h"
#include "ShaderManager.h"
#include "BufferManager.h"
#include <filesystem>

ShaderManager::ShaderManager(SDL_GPUDevice* device) {
    dev = device;

    SDL_ShaderCross_Init();

    // === НАДЁЖНЫЙ путь к папке кэша ===
    const char* base = SDL_GetBasePath();                    // папка, где лежит .exe

    m_cacheBasePath = std::string(base) + "shaders/shader_cache";

    std::filesystem::create_directories(m_cacheBasePath);
    SDL_Log("[Shader] Shader cache directory: %s", m_cacheBasePath.c_str());
    SDL_free((void*)base);
};

ShaderProgram* ShaderManager::CreateShaderProgram(
    const std::string& name, const ShaderProgramDescription& spd, RenderPassStep* associated_pass,
    const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
    const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
    const std::vector<TextureSlotRole>& texture_slots)
{
    auto it = shader_programs.find(name);
    if (it != shader_programs.end()) {
        SDL_Log("Shader program '%s' already exists, returning existing program.", name.c_str());
        return it->second.get();
    }

    auto program = std::make_unique<ShaderProgram>();
    program->vs_name = vs_name;   // ссылки по имени на реестры vertex_shaders/fragment_shaders
    program->fs_name = fs_name;
	program->vertex_shader_buffer_names = std::move(vertex_shader_buffer_names);
	program->fragment_shader_buffer_names = std::move(fragment_shader_buffer_names);
	// Слот-роли ВЗАИМОИСКЛЮЧАЮЩИЕ: одна роль = один слот текстуры. Повтор — ошибка композиции
	// (материал держит одну текстуру на роль); отсеиваем дубликаты, оставляя первое вхождение.
	program->required_slots.reserve(texture_slots.size());
	for (TextureSlotRole role : texture_slots) {
		if (std::find(program->required_slots.begin(), program->required_slots.end(), role) != program->required_slots.end()) {
			SDL_Log("ShaderManager::CreateShaderProgram '%s': duplicate texture slot role %d — skipped", name.c_str(), static_cast<int>(role));
			continue;
		}
		program->required_slots.push_back(role);
	}
	program->spd = spd;
    program->associated_render_pass = associated_pass;
    ShaderProgram* ptr = program.get();

    shader_programs.emplace(name, std::move(program));

	dirty_graphics_pipelines = true;
    return ptr;
}

ComputeShaderProgram* ShaderManager::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
    std::vector<BufferData*> rw_storage_buffers, std::vector<BufferData*> ro_storage_buffers,
    std::vector<ComputeShaderProgram::ComputeRWTextureBinding> rw_storage_textures,
    std::vector<TextureAtlas*> ro_storage_textures,
    std::vector<TextureAtlas*> texture_samplers,
    ComputePassStep* associated_compute_pass)
{
    auto it = compute_shader_programs_by_name.find(name);
    if (it != compute_shader_programs_by_name.end()) {
        SDL_Log("Compute shader program '%s' already exists, returning existing.", name.c_str());
        return it->second;
    }

    auto result = std::make_unique<ComputeShaderProgram>();
    result->cs_name = cs_name;   // ссылка по имени на реестр compute_shaders
    result->associated_compute_pass = associated_compute_pass;

    result->ro_storage_buffers = std::move(ro_storage_buffers);
    result->rw_storage_buffers = std::move(rw_storage_buffers);
    result->rw_storage_textures = std::move(rw_storage_textures);

    result->ro_storage_textures = std::move(ro_storage_textures);
    result->texture_samplers = std::move(texture_samplers);

    result->debug_name = name;

    ComputeShaderProgram* ptr = result.get();
    compute_shader_programs.push_back(std::move(result));
    compute_shader_programs_by_name.emplace(name, ptr);

    dirty_compute_pipelines = true;
    dirty_compute_batches = true;
    return ptr;
}

ShaderProgram* ShaderManager::GetShaderProgram(const std::string& name)
{
    auto it = shader_programs.find(name);
    if (it != shader_programs.end())
        return it->second.get();
    SDL_Log("Shader program '%s' not found", name.c_str());
    return nullptr;
}

// Резолв именованных шейдер-данных (без лога на промахе — зовётся на каждой сборке пайплайна/батча).
VertexShaderData* ShaderManager::GetVertexShader(const std::string& name)
{
    auto it = vertex_shaders.find(name);
    return it != vertex_shaders.end() ? &it->second : nullptr;
}

FragmentShaderData* ShaderManager::GetFragmentShader(const std::string& name)
{
    auto it = fragment_shaders.find(name);
    return it != fragment_shaders.end() ? &it->second : nullptr;
}

ComputeShaderData* ShaderManager::GetComputeShader(const std::string& name)
{
    auto it = compute_shaders.find(name);
    return it != compute_shaders.end() ? &it->second : nullptr;
}

bool ShaderManager::DeleteComputeShader(const std::string& name)
{
    if (IsComputeShaderUsed(name)) {   // см. комментарий у DeleteVertexShader
        SDL_Log("ShaderManager: compute shader '%s' is used by a compute program — delete refused", name.c_str());
        return false;
    }
    auto it = compute_shaders.find(name);
    if (it == compute_shaders.end()) return false;
    if (it->second.spv_code) SDL_free(it->second.spv_code);   // владелец сырого spv — реестр
    compute_shaders.erase(it);
    return true;
}

ComputeShaderProgram* ShaderManager::GetComputeShaderProgram(const std::string& name)
{
    auto it = compute_shader_programs_by_name.find(name);
    if (it != compute_shader_programs_by_name.end())
        return it->second;
    SDL_Log("Compute shader data '%s' not found", name.c_str());
	return nullptr;
}

ShaderManager::~ShaderManager()
{
	// GPU-шейдеры освобождаются по refcount (shared_ptr в ShaderData) при shader_programs.clear()
	// ниже — без явного SDL_ReleaseGPUShader, иначе шарящийся vs (main+transparent через один
	// main_pass_vs) словил бы double-free.
    for (auto& [n, cs] : compute_shaders) {   // владелец spv_code теперь реестр, не csp
        if (cs.spv_code) SDL_free(cs.spv_code);
	}
	shader_programs.clear();   // sp умирают → их shared_ptr отпускаются (device ещё жив)
	shader_alive_.reset();     // токен гасим ПОСЛЕ: поздние релизы (статик vs на выходе) → no-op
	SDL_ShaderCross_Quit();
}

ShaderProgramDescription* ShaderProgramDescription::BehavesAsShadowCaster() {
    depth_test = true;  depth_write = true;  stencil_test = false;
    color_blend = false;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}
ShaderProgramDescription* ShaderProgramDescription::BehavesAsOpaqueGeometry() {
    depth_test = true;  depth_write = true;
    color_blend = false;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}
ShaderProgramDescription* ShaderProgramDescription::BehavesAsTransparentGeometry() {
    depth_test = true;  depth_write = false;
    color_blend = true;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}
ShaderProgramDescription* ShaderProgramDescription::BehavesAsDepthPrepass() {
    depth_test = true;  depth_write = true;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}
ShaderProgramDescription* ShaderProgramDescription::BehavesAsFullscreenEffect() {
    depth_test = false; depth_write = false;
    color_blend = false;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}