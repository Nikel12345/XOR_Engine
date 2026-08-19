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

    const char* base = SDL_GetBasePath();                    // папка, где лежит .exe

    m_cacheBasePath = std::string(base) + "shaders/shader_cache";

    std::filesystem::create_directories(m_cacheBasePath);
    SDL_Log("[Shader] Shader cache directory: %s", m_cacheBasePath.c_str());
    // НЕ освобождать base: в SDL3 (в отличие от SDL2) строка SDL_GetBasePath принадлежит SDL
    // (кэш, освобождается в SDL_Quit). SDL_free здесь = double free → heap corruption на выходе
    // у любого процесса, который корректно зовёт SDL_Quit (зонды песочницы).
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
    program->vs_name = vs_name;   // ссылки по имени на реестры vertex_shaders/fragment_shaders
    program->fs_name = fs_name;
	program->vertex_shader_buffer_names = std::move(vertex_shader_buffer_names);
	program->fragment_shader_buffer_names = std::move(fragment_shader_buffer_names);
	// Слот-роли ВЗАИМОИСКЛЮЧАЮЩИЕ: одна роль = один слот текстуры. Повтор — ошибка композиции
	// (материал держит одну текстуру на роль); отсеиваем дубликаты, оставляя первое вхождение.
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
    program->debug_name = name;   // только для логов (см. ShaderProgram::debug_name)

    // ── Сбор usage-флагов ──
    // Storage-буферы обеих стадий биндятся через SDL_BindGPUVertex/FragmentStorageBuffers, а те
    // требуют GRAPHICS_STORAGE_READ (SDL_gpu.h:3054, :3127). Union, без приоритетов.
    // texture_slots (роли) НЕ дают флага атласу: материал ссылается на текстуру по имени, и в какой
    // она атлас — выясняется лишь на сборке батча, уже после бейка. Это ожидаемое расхождение.
    if (bm) {
        auto collect = [bm](const std::vector<BufferDataName>& names) {
            for (BufferDataName n : names)
                if (BufferData* bd = bm->GetBufferData(n))
                    bd->usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        };
        collect(program->vertex_shader_buffer_names);
        collect(program->fragment_shader_buffer_names);
    }

    // Код-байндинг по имени — сразу на создании, если он уже зарегистрирован (CreatePushFunc).
    // Так порядок «функция / программа» не значит ничего, и sp, пересозданная загрузкой сцены
    // или редактором, приходит в мир уже с push_func, а не голой до ближайшего BindShaderFunctions.
    if (auto pit = push_instructions_.find(name); pit != push_instructions_.end())
        program->push_func = pit->second;

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
    result->cs_name = cs_name;   // ссылка по имени на реестр compute_shaders
    result->compute_pass_name = compute_pass_name;
    result->debug_name = name;   // только для логов (см. ComputeShaderProgram::debug_name)

    result->dont_save = dont_save;

    result->ro_storage_buffer_names = std::move(ro_storage_buffers);
    result->rw_storage_buffer_names = std::move(rw_storage_buffers);
    result->rw_storage_textures = std::move(rw_storage_textures);

    result->ro_storage_texture_names = std::move(ro_storage_textures);
    result->texture_sampler_names = std::move(texture_samplers);


    // ── Сбор usage-флагов ──
    // Роль задаёт СПИСОК, в котором ресурс объявлен, — каждый SDL_Bind* проверяет свой бит:
    //   ro-буферы  → SDL_BindGPUComputeStorageBuffers требует COMPUTE_STORAGE_READ   (SDL_gpu.h:3375)
    //   rw-буферы  → SDL_GPUStorageBufferReadWriteBinding требует COMPUTE_STORAGE_WRITE (:2038)
    //   ro/rw-текстуры и сэмплеры — то же самое для текстурных флагов.
    // Union без приоритетов: RO и RW НЕЗАВИСИМЫ. «rw важнее ro» было бы ошибкой — уронив RO ради
    // RW, мы сломали бы бинд той программы, что читает этот же буфер как RO.
    // SIMULTANEOUS_READ_WRITE — не выводится из формы бинда, а берётся из РУЧНОГО тега
    // need_simultaneous на самом rw-биндинге: это факт о теле шейдера (читает ли он соседние
    // тексели, пока другие потоки их пишут), а bloom_up и bloom_composite регистрируются
    // одинаково. См. ComputeRWTextureBindingParametr::need_simultaneous.
    // Резолв ТОЛЬКО ради флагов (сама программа хранит имена). Промах здесь не отменяет создание:
    // ресурс может появиться позже — тогда батч его и найдёт, а вот флаг уже опоздает, см.
    // warnings.md («usage-флаг после бейка»).
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

    // См. CreateShaderProgram: зарегистрированные push/dispatch вешаются сразу.
    if (auto pit = compute_push_instructions_.find(name); pit != compute_push_instructions_.end())
        result->push_func = pit->second;
    if (auto dit = dispatch_instructions_.find(name); dit != dispatch_instructions_.end())
        result->dispatch_func = dit->second;

    ComputeShaderProgram* ptr = result.get();
    compute_shader_programs.push_back({ name, std::move(result) });

    dirty_compute_pipelines = true;
    dirty_compute_batches = true;
    return ptr;
}

// ── Реестр код-байндингов ───────────────────────────────────────────────────────────────────
// Регистрация двусторонняя: кладём запись И тут же вешаем её на программу, если та уже есть.
// Вместе с привязкой на создании программы это даёт инвариант «push_func программы == запись
// реестра под её именем» без единой точки синхронизации.

void ShaderManager::CreatePushFunc(const std::string& sp_name, PushFunc fn)
{
    auto& slot = push_instructions_[sp_name] = std::move(fn);   // 1 программа — 1 функция (перезапись)
    if (auto it = shader_programs.find(sp_name); it != shader_programs.end())
        it->second->push_func = slot;
}

void ShaderManager::CreateComputePushFunc(const std::string& csp_name, PushFunc fn)
{
    auto& slot = compute_push_instructions_[csp_name] = std::move(fn);
    if (ComputeShaderProgram* csp = GetComputeShaderProgram(csp_name)) csp->push_func = slot;
}

void ShaderManager::CreateDispatchFunc(const std::string& csp_name, DispatchFunc fn)
{
    auto& slot = dispatch_instructions_[csp_name] = std::move(fn);
    if (ComputeShaderProgram* csp = GetComputeShaderProgram(csp_name)) csp->dispatch_func = slot;
}

void ShaderManager::BindShaderFunctions()
{
    // Осиротевшие записи — не ошибка: сцена могла просто не привезти свою программу (у каждого
    // фрактала свой sp, а функции обоих зарегистрированы разом в Init). Но это ровно тот случай,
    // который разовый колбэк с if (sp) съедал молча, — поэтому он идёт в лог одной строкой.
    // set, а не строка: push и dispatch — РАЗНЫЕ реестры, и csp без программы попадала в отчёт
    // дважды, что читалось как удвоение списка.
    std::set<std::string> orphans;
    auto bind = [&orphans](auto& instructions, auto&& lookup, auto&& assign) {
        for (auto& [name, fn] : instructions) {
            if (auto* prog = lookup(name)) assign(prog, fn);
            else orphans.insert(name);
        }
    };

    bind(push_instructions_,
        [this](const std::string& n) -> ShaderProgram* {
            auto it = shader_programs.find(n);
            return it != shader_programs.end() ? it->second.get() : nullptr;
        },
        [](ShaderProgram* sp, const PushFunc& fn) { sp->push_func = fn; });

    auto find_csp = [this](const std::string& n) { return GetComputeShaderProgram(n); };
    bind(compute_push_instructions_, find_csp,
        [](ComputeShaderProgram* csp, const PushFunc& fn) { csp->push_func = fn; });
    bind(dispatch_instructions_, find_csp,
        [](ComputeShaderProgram* csp, const DispatchFunc& fn) { csp->dispatch_func = fn; });

    if (!orphans.empty()) {
        std::string list;
        for (const std::string& n : orphans) { if (!list.empty()) list += ", "; list += n; }
        SDL_Log("ShaderManager: push/dispatch funcs without a program: %s", list.c_str());
    }
}

void ShaderManager::ClearSavableComputeShaderPrograms()
{
    const size_t before = compute_shader_programs.size();
    // Порядок уцелевших сохраняем — он же порядок их исполнения.
    std::erase_if(compute_shader_programs,
        [](const ComputeProgramSlot& s) { return !s.program || !s.program->dont_save; });
    const size_t removed = before - compute_shader_programs.size();
    if (removed) {
        dirty_compute_pipelines = true;
        dirty_compute_batches = true;
        SDL_Log("ShaderManager: %zu savable compute shader programs cleared", removed);
    }
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
        SDL_Log("ShaderManager: compute shader '%s' is used by a compute program - delete refused", name.c_str());
        return false;
    }
    auto it = compute_shaders.find(name);
    if (it == compute_shaders.end()) return false;
    if (it->second.spv_code) SDL_free(it->second.spv_code);   // владелец сырого spv — реестр
    compute_shaders.erase(it);
    return true;
}

// Линейный поиск по единственному реестру: программ десятки, а все вызовы — холодные (создание,
// регистрация код-байндингов, редактор). Отдельный индекс по имени был бы вторым источником истины.
ComputeShaderProgram* ShaderManager::GetComputeShaderProgram(const std::string& name)
{
    for (auto& slot : compute_shader_programs)
        if (slot.name == name) return slot.program.get();
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
// UI-оверлей: перекрытие решает Z (depth_test+write ON), прозрачность — блендом, а прозрачные
// пиксели ОТБРАСЫВАЮТСЯ в шейдере (clip) — иначе depth_write запечатал бы дыры. См. ui.frag.
ShaderProgramDescription* ShaderProgramDescription::BehavesAsUIOverlay() {
    depth_test = true;  depth_write = true;
    color_blend = true;
    cull_mode = SDL_GPU_CULLMODE_NONE;
    return this;
}