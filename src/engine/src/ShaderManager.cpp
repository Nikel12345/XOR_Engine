#include "PCH.h"
#include "ShaderManager.h"
#include "BufferManager.h"
#include "TextureManager.h"
#include "TextureData.h"
#include <filesystem>
#include <set>

ShaderManager::ShaderManager(SDL_GPUDevice* device) {
    dev = device;

    SDL_ShaderCross_Init();

    const char* base = SDL_GetBasePath();

    m_cacheBasePath = std::string(base) + "shaders/shader_cache";

    std::filesystem::create_directories(m_cacheBasePath);
    SDL_Log("[Shader] Shader cache directory: %s", m_cacheBasePath.c_str());
    // base НЕ освобождать: в SDL3 строка принадлежит SDL, и SDL_free здесь даёт double free
    // на выходе у любого процесса, который зовёт SDL_Quit.
};

ShaderProgram* ShaderManager::CreateShaderProgram(
    const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& render_pass_name,
    const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
    const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
    const std::vector<TextureSlotRole>& texture_slots, BufferManager* bm)
{
    auto it = shader_programs.find(name);
    if (it != shader_programs.end()) {
        SDL_Log("Shader program '%s' already exists, returning existing program.", name.c_str());
        return it->second.get();
    }

    auto program = std::make_unique<ShaderProgram>();
    program->vs_name = vs_name;
    program->fs_name = fs_name;
	program->vertex_shader_buffer_names = std::move(vertex_shader_buffer_names);
	program->fragment_shader_buffer_names = std::move(fragment_shader_buffer_names);
	program->required_slots.reserve(texture_slots.size());
	for (TextureSlotRole role : texture_slots) {
		if (std::find(program->required_slots.begin(), program->required_slots.end(), role) != program->required_slots.end()) {
			SDL_Log("ShaderManager::CreateShaderProgram '%s': duplicate texture slot role %d - skipped", name.c_str(), static_cast<int>(role));
			continue;
		}
		program->required_slots.push_back(role);
	}
	program->spd = spd;
    program->render_pass_name = render_pass_name;
    program->debug_name = name;


    if (bm) {
        auto collect = [bm](const std::vector<BufferDataName>& names) {
            for (BufferDataName n : names)
                if (BufferData* bd = bm->GetBufferData(n))
                    bd->usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        };
        collect(program->vertex_shader_buffer_names);
        collect(program->fragment_shader_buffer_names);
    }

    ShaderProgram* ptr = program.get();

    shader_programs.emplace(name, std::move(program));

	dirty_graphics_pipelines = true;
    return ptr;
}

ComputeShaderProgram* ShaderManager::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
    std::vector<BufferDataName> rw_storage_buffers, std::vector<BufferDataName> ro_storage_buffers,
    std::vector<ComputeRWTextureBindingParametr> rw_storage_textures,
    std::vector<AtlasName> ro_storage_textures,
    std::vector<AtlasName> texture_samplers,
    const ComputePassName& compute_pass_name,
    BufferManager* bm, TextureManager* tm, bool dont_save)
{
    if (ComputeShaderProgram* existing = GetComputeShaderProgram(name)) {
        SDL_Log("Compute shader program '%s' already exists, returning existing.", name.c_str());
        return existing;
    }

    auto result = std::make_unique<ComputeShaderProgram>();
    result->cs_name = cs_name;
    result->compute_pass_name = compute_pass_name;
    result->debug_name = name;

    result->dont_save = dont_save;

    result->ro_storage_buffer_names = std::move(ro_storage_buffers);
    result->rw_storage_buffer_names = std::move(rw_storage_buffers);
    result->rw_storage_textures = std::move(rw_storage_textures);

    result->ro_storage_texture_names = std::move(ro_storage_textures);
    result->texture_sampler_names = std::move(texture_samplers);

    auto buf = [bm](BufferDataName n) -> BufferData* {
        if (!bm) return nullptr;
        BufferData* bd = bm->GetBufferData(n);
        if (!bd) SDL_Log("ShaderManager::CreateComputeShaderProgram: storage buffer '%s' not found - usage flag not declared", n);
        return bd;
    };
    auto atlas = [tm](const AtlasName& n) -> TextureAtlas* {
        if (!tm) return nullptr;
        TextureAtlas* a = tm->GetTextureAtlas(n);
        if (!a) SDL_Log("ShaderManager::CreateComputeShaderProgram: texture atlas '%s' not found - usage flag not declared", n.c_str());
        return a;
    };

    for (BufferDataName n : result->ro_storage_buffer_names)
        if (BufferData* bd = buf(n)) bd->usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    for (BufferDataName n : result->rw_storage_buffer_names)
        if (BufferData* bd = buf(n)) bd->usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

    for (const AtlasName& n : result->ro_storage_texture_names)
        if (TextureAtlas* a = atlas(n)) a->tci.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    for (const ComputeRWTextureBindingParametr& b : result->rw_storage_textures) {
        TextureAtlas* a = atlas(b.texture_atlas);
        if (!a) continue;
        a->tci.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        if (b.need_simultaneous)
            a->tci.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    }
    for (const AtlasName& n : result->texture_sampler_names)
        if (TextureAtlas* a = atlas(n)) a->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;


    ComputeShaderProgram* ptr = result.get();
    compute_shader_programs.push_back({ name, std::move(result) });

    dirty_compute_pipelines = true;
    dirty_compute_batches = true;
    return ptr;
}


void ShaderManager::CreatePushInstruction(const std::string& sp_name, PushStage stage, PushFunc fn)
{
    push_instructions_.push_back({ sp_name, stage, std::move(fn) });
}

void ShaderManager::CreateComputePushInstruction(const std::string& csp_name, PushFunc fn)
{
    compute_push_instructions_.push_back({ csp_name, PushStage::Compute, std::move(fn) });
}

void ShaderManager::CreateDispatchInstruction(const std::string& csp_name, DispatchFunc fn)
{
    dispatch_instructions_[csp_name] = std::move(fn);
}

namespace {
    struct SlotCounter {
        Uint32 next[3] = { 0, 0, 0 };   // по индексу PushStage
        void Add(PushInstructions& out, PushStage stage, const PushFunc& fn) {
            out.push_back({ stage, next[static_cast<size_t>(stage)]++, fn });
        }
    };
}

void ShaderManager::AddKindInstructions(PushInstructions& out, void* slots_raw,
    const std::vector<std::string>& kinds, const std::string& owner) const
{
    SlotCounter& slots = *static_cast<SlotCounter*>(slots_raw);
    for (const std::string& kind : kinds) {
        auto it = push_kinds_.find(kind);
        if (it == push_kinds_.end()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "PushKind '%s' (объявлен маркером в '%s') не зарегистрирован - слот занят пустышкой, "
                "блок останется с данными прошлого draw'а", kind.c_str(), owner.c_str());
            slots.Add(out, PushStage::Fragment, PushFunc{});
            continue;
        }
        slots.Add(out, it->second.stage, it->second.fn);
    }
}

PushInstructions ShaderManager::CollectPushInstructions(const std::string& sp_name) const
{
    PushInstructions out;
    SlotCounter slots;

    if (auto sit = shader_programs.find(sp_name); sit != shader_programs.end()) {
        const ShaderProgram* sp = sit->second.get();
        if (auto vit = vertex_shaders.find(sp->vs_name); vit != vertex_shaders.end())
            AddKindInstructions(out, &slots, vit->second.push_kinds, sp->vs_name);
        if (auto fit = fragment_shaders.find(sp->fs_name); fit != fragment_shaders.end())
            AddKindInstructions(out, &slots, fit->second.push_kinds, sp->fs_name);
    }

    for (const ShaderPushInstruction& instr : push_instructions_)
        if (instr.program_name == sp_name) slots.Add(out, instr.stage, instr.fn);

    if (auto sit = shader_programs.find(sp_name); sit != shader_programs.end()) {
        if (auto fit = fragment_shaders.find(sit->second->fs_name); fit != fragment_shaders.end()) {
            const Uint32 declared = fit->second.shader_data.num_uniform_buffers;
            if (slots.next[static_cast<size_t>(PushStage::Fragment)] != declared)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "sp '%s': fragment-инструкций %u, а шейдер '%s' объявил %u uniform-блоков "
                    "- проверь маркеры //@push (порядок и наличие)",
                    sp_name.c_str(), slots.next[static_cast<size_t>(PushStage::Fragment)],
                    sit->second->fs_name.c_str(), declared);
        }
    }

    return out;
}

PushInstructions ShaderManager::CollectComputePushInstructions(const std::string& csp_name) const
{
    PushInstructions out;
    SlotCounter slots;

    if (ComputeShaderProgram* csp = const_cast<ShaderManager*>(this)->GetComputeShaderProgram(csp_name))
        if (auto cit = compute_shaders.find(csp->cs_name); cit != compute_shaders.end())
            AddKindInstructions(out, &slots, cit->second.push_kinds, csp->cs_name);

    for (const ShaderPushInstruction& instr : compute_push_instructions_)
        if (instr.program_name == csp_name) slots.Add(out, instr.stage, instr.fn);

    return out;
}

void ShaderManager::RegisterPushKind(const std::string& kind, PushStage stage, PushFunc fn)
{
    push_kinds_[kind] = PushKind{ stage, std::move(fn) };
}

ShaderManager::DispatchFunc ShaderManager::GetDispatchInstruction(const std::string& csp_name) const
{
    auto it = dispatch_instructions_.find(csp_name);
    return it != dispatch_instructions_.end() ? it->second : DispatchFunc{};
}

void ShaderManager::ReportOrphanCodeBindings()
{
    std::set<std::string> orphans;
    auto check_named = [&orphans](const auto& instructions, auto&& lookup) {
        for (const auto& instr : instructions)
            if (!lookup(instr.program_name)) orphans.insert(instr.program_name);
    };

    auto find_sp = [this](const std::string& n) -> ShaderProgram* {
        auto it = shader_programs.find(n);
        return it != shader_programs.end() ? it->second.get() : nullptr;
    };
    auto find_csp = [this](const std::string& n) { return GetComputeShaderProgram(n); };

    check_named(push_instructions_, find_sp);
    check_named(compute_push_instructions_, find_csp);
    for (const auto& [name, fn] : dispatch_instructions_)
        if (!find_csp(name)) orphans.insert(name);

    if (!orphans.empty()) {
        std::string list;
        for (const std::string& n : orphans) { if (!list.empty()) list += ", "; list += n; }
        SDL_Log("ShaderManager: push/dispatch funcs without a program: %s", list.c_str());
    }
}

void ShaderManager::ClearSavableComputeShaderPrograms()
{
    const size_t before = compute_shader_programs.size();
    std::erase_if(compute_shader_programs,
        [](const ComputeProgramSlot& s) { return !s.program || !s.program->dont_save; });
    const size_t removed = before - compute_shader_programs.size();
    if (removed) {
        dirty_compute_pipelines = true;
        dirty_compute_batches = true;
        SDL_Log("ShaderManager: %zu savable compute shader programs cleared", removed);
    }
}

ShaderProgram* ShaderManager::GetShaderProgram(const ShaderName& name)
{
    auto it = shader_programs.find(name);
    if (it != shader_programs.end())
        return it->second.get();
    SDL_Log("Shader program '%s' not found", name.c_str());
    return nullptr;
}

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
    if (IsComputeShaderUsed(name)) {
        SDL_Log("ShaderManager: compute shader '%s' is used by a compute program - delete refused", name.c_str());
        return false;
    }
    auto it = compute_shaders.find(name);
    if (it == compute_shaders.end()) return false;
    if (it->second.spv_code) SDL_free(it->second.spv_code);
    compute_shaders.erase(it);
    return true;
}

ComputeShaderProgram* ShaderManager::GetComputeShaderProgram(const std::string& name)
{
    for (auto& slot : compute_shader_programs)
        if (slot.name == name) return slot.program.get();
	return nullptr;
}

ShaderManager::~ShaderManager()
{
    for (auto& [n, cs] : compute_shaders) {
        if (cs.spv_code) SDL_free(cs.spv_code);
	}
	// Явного SDL_ReleaseGPUShader нет: шарящийся vs словил бы double-free. Шейдеры отпускают
	// реестры при разрушении членов — device к этому моменту ещё жив (см. ~Engine).
	shader_programs.clear();
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
// UI-оверлей: перекрытие решает Z (depth_test+write ON), прозрачность — блендом, а прозрачные
// пиксели ОТБРАСЫВАЮТСЯ в шейдере (clip) — иначе depth_write запечатал бы дыры.
ShaderProgramDescription* ShaderProgramDescription::BehavesAsUIOverlay() {
    depth_test = true;  depth_write = true;
    color_blend = true;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}