#include "PCH.h"
#include "ShaderManager.h"
#include <string_view>

inline uint64_t HashBytes(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// FNV-1a, продолжаемый: домешивает байты в уже накопленный hash (а не начинает с нуля).
static inline void FnvMix(uint64_t& hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
}

// Рекурсивно домешивает в hash содержимое файла и всех его #include "..." . Инклуды движка
// пишутся root-relative и резолвятся от include_dir (как делает сам SDL_ShaderCross при
// компиляции). visited защищает от циклов (наш сканер не видит include-guard'ы) и от
// повторного учёта файла, включённого из нескольких мест. Так правка ЛЮБОГО файла по цепочке
// инклудов меняет хэш → кэш .spv корректно инвалидируется (раньше хэшировался только верхний файл).
static void HashIncludesRecursive(uint64_t& hash, const std::string& path,
    const char* include_dir, std::unordered_set<std::string>& visited)
{
    if (!visited.insert(path).second) return;

    size_t size = 0;
    char* data = (char*)SDL_LoadFile(path.c_str(), &size);
    if (!data) return;   // файл не найден — пропускаем; реальную ошибку выдаст компиляция
    FnvMix(hash, (const uint8_t*)data, size);

    std::string_view sv(data, size);
    for (size_t pos = 0; (pos = sv.find("#include", pos)) != std::string_view::npos; ) {
        size_t q1 = sv.find('"', pos + 8);
        size_t nl = sv.find('\n', pos + 8);
        // Кавычка обязана быть на той же строке, что и #include — иначе это не директива.
        if (q1 == std::string_view::npos || (nl != std::string_view::npos && q1 > nl)) { pos += 8; continue; }
        size_t q2 = sv.find('"', q1 + 1);
        if (q2 == std::string_view::npos) break;
        std::string rel(sv.substr(q1 + 1, q2 - q1 - 1));
        HashIncludesRecursive(hash, std::string(include_dir) + "/" + rel, include_dir, visited);
        pos = q2 + 1;
    }
    SDL_free(data);
}

std::string ShaderManager::BuildCachePath(const char* source_path, uint64_t hash) const
{
    // Имя файла без пути
    const char* filename = source_path;
    for (const char* p = source_path; *p; ++p) {
        if (*p == '/' || *p == '\\') filename = p + 1;
    }

    char hash_str[17];
    SDL_snprintf(hash_str, sizeof(hash_str), "%016llx", (unsigned long long)hash);

    return m_cacheBasePath + "/" + filename + "." + hash_str + ".spv";
}

void ShaderManager::ReadVertexAttributes(
    std::initializer_list<VertexBufferBinding> bindings,
    VertexShaderData& vs)
{
    vs.attributes.clear(); vs.vbs.clear();

    Uint32 slot = 0;
    for (auto& g : bindings) {
        for (VertexSemantic sem : g.pull) {
            const VertexAttr* a = g.format->Find(sem);
            if (!a) {
                SDL_Log("Warning: Vertex format for buffer '%s' does not contain semantic %u, skipping this attribute.",
					g.buffer, (Uint32)sem);
                continue;
			}

            SDL_GPUVertexAttribute attr{};
            attr.location = (Uint32)sem;   // = [[vk::location]] в шейдере
            attr.buffer_slot = slot;
            attr.format = a->format;
            attr.offset = a->offset; 
            vs.attributes.push_back(attr);
        }
        SDL_GPUVertexBufferDescription vb{};
        vb.slot = slot;
        vb.pitch = g.format->stride;          // настоящий sizeof
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX; vb.instance_step_rate = 0;
        vs.vbs.push_back(vb);
        ++slot;
    }
}

Uint8* ShaderManager::LoadOrCompileSPIRV(const char* hlsl_path,
    SDL_ShaderCross_ShaderStage stage,
    size_t& out_size)
{
    size_t src_size = 0;
    char* src = (char*)SDL_LoadFile(hlsl_path, &src_size);
    if (!src) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load HLSL: %s", hlsl_path);
        return nullptr;
    }

    // === Хэш: верхний файл + рекурсивно все его #include + целевая платформа/GPU ===
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(dev);
    const char* include_dir = "../engine/shaders_code";   // один источник истины (см. ниже hlsl_info)

    // Раньше хэшировался ТОЛЬКО верхний файл (src) → правки инклудов (база/прологи) кэш не
    // сбрасывали и грузился устаревший .spv. Теперь обходим всю цепочку #include.
    uint64_t hash = 14695981039346656037ULL;
    {
        std::unordered_set<std::string> visited;
        HashIncludesRecursive(hash, hlsl_path, include_dir, visited);
    }

    // GPU-формат (DXIL / MSL / SPIRV) — чтобы при смене видеокарты/ОС кэш пересобрался.
    hash ^= (uint64_t)supported;
    hash *= 1099511628211ULL;   // чтобы изменение формата сильно меняло хэш

    std::string cache_path = BuildCachePath(hlsl_path, hash);

    // Пробуем кэш
    Uint8* spv = (Uint8*)SDL_LoadFile(cache_path.c_str(), &out_size);
    if (spv) {
        SDL_Log("[Shader] Cache hit: %s", cache_path.c_str());
        SDL_free(src);
        return spv;
    }

    // Компилируем HLSL → SPIR-V
    SDL_ShaderCross_HLSL_Info hlsl_info{};
    hlsl_info.source = src;
    hlsl_info.entrypoint = "main";
    hlsl_info.shader_stage = stage;
    hlsl_info.include_dir = include_dir;
    hlsl_info.defines = nullptr;
    hlsl_info.props = 0;

    size_t compiled_size = 0;
    void* compiled = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &compiled_size);
    SDL_free(src);

    if (!compiled) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "ShaderCross compile failed: %s\n%s", hlsl_path, SDL_GetError());
        return nullptr;
    }

    // Сохраняем в кэш
    SDL_IOStream* f = SDL_IOFromFile(cache_path.c_str(), "wb");
    if (f) {
        SDL_WriteIO(f, compiled, compiled_size);
        SDL_CloseIO(f);
        SDL_Log("[Shader] Compiled and cached: %s", cache_path.c_str());
    }

    out_size = compiled_size;
    return (Uint8*)compiled;
}

VertexShaderData ShaderManager::CreateVertexShader(const char* hlsl_path,
    std::initializer_list<VertexBufferBinding> bindings)
{
    size_t n = 0;
    Uint8* spv = LoadOrCompileSPIRV(hlsl_path, SDL_SHADERCROSS_SHADERSTAGE_VERTEX, n);
    if (!spv) return {};
    VertexShaderData vs = BuildVertexShader(spv, n, hlsl_path, bindings);
    SDL_free(spv);
    return vs;
}

FragmentShaderData ShaderManager::CreateFragmentShader(const char* hlsl_path)
{
    size_t n = 0;
    Uint8* spv = LoadOrCompileSPIRV(hlsl_path, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, n);
    if (!spv) return {};
    FragmentShaderData fs = BuildFragmentShader(spv, n, hlsl_path);
    SDL_free(spv);
    return fs;
}

ComputeShaderData ShaderManager::CreateComputeShader(const char* hlsl_path)
{
    size_t n = 0;
    Uint8* spv = LoadOrCompileSPIRV(hlsl_path, SDL_SHADERCROSS_SHADERSTAGE_COMPUTE, n);
    if (!spv) return {};
    return BuildComputeShader(spv, n, hlsl_path);   // владение spv уходит в cs
}

std::shared_ptr<SDL_GPUShader> ShaderManager::LookupGpuShader(uint64_t key) const
{
    auto it = gpu_shaders.find(key);
    if (it != gpu_shaders.end())
        if (auto live = it->second.lock()) return live;   // живой дубль → переиспользуем
    return nullptr;
}

std::shared_ptr<SDL_GPUShader> ShaderManager::RegisterGpuShader(uint64_t key, SDL_GPUShader* raw)
{
    // Делитер освобождает GPU-шейдер при refcount→0, но ТОЛЬКО пока жив токен менеджера (а с
    // ним device). Поздний релиз (статик-копия vs на выходе из программы) → no-op, ресурс
    // добьёт уничтожение device. Так избегаем SDL_ReleaseGPUShader по мёртвому device.
    std::shared_ptr<SDL_GPUShader> sh(raw,
        [dev = dev, token = std::weak_ptr<int>(shader_alive_)](SDL_GPUShader* s) {
            if (s && !token.expired()) SDL_ReleaseGPUShader(dev, s);
        });
    gpu_shaders[key] = sh;   // weak-индекс для дедупа
    return sh;
}

VertexShaderData ShaderManager::BuildVertexShader(
    const Uint8* spv, size_t spv_size, const char* dbg_name,
    std::initializer_list<VertexBufferBinding> bindings)
{
    VertexShaderData vs{};
    ReadVertexAttributes(bindings, vs);   // раскладка из VertexFormat, рефлексия не нужна

    // Дедуп: одинаковый SPIR-V → один GPU-шейдер на всех владельцев (refcount через shared_ptr).
    const uint64_t key = HashBytes(spv, spv_size) ^ (uint64_t)SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
    if (auto cached = LookupGpuShader(key)) { vs.shader_data.shader = cached; return vs; }

    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV(spv, spv_size, 0);
    if (!metadata) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to reflect vertex shader: %s", dbg_name);
        return {};
    }

    for (Uint32 i = 0; i < metadata->num_inputs; ++i) {
        const auto& in = metadata->inputs[i];
        bool ok = false;
        for (const auto& a : vs.attributes) if (a.location == in.location) { ok = true; break; }
        if (!ok) {
            SDL_Log("Warning: VS '%s': input location %u not provided by vertex buffer bindings (pull / [[vk::location]]), this attribute will be missing in the shader.",
				dbg_name, in.location);
            SDL_assert(false);
        }
    }

    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = spv; info.bytecode_size = spv_size;
    info.entrypoint = "main"; info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX; info.props = 0;
    SDL_GPUShader* raw = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &info, &metadata->resource_info, 0);

    SDL_free(metadata);
    if (!raw) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GPU vertex shader: %s", dbg_name);
        return {};
    }
    vs.shader_data.shader = RegisterGpuShader(key, raw);
    return vs;
}

FragmentShaderData ShaderManager::BuildFragmentShader(
    const Uint8* spv, size_t spv_size, const char* dbg_name)
{
    FragmentShaderData fs{};

    // Рефлексия ВСЕГДА (дёшево, load-time): нужна для числа uniform-буферов (гейт пуша params
    // в RenderManager) и для компиляции. Дедуп ниже только переиспользует готовый GPU-шейдер.
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV(spv, spv_size, 0);
    if (!metadata) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to reflect fragment shader: %s", dbg_name);
        return {};
    }
    fs.shader_data.num_uniform_buffers = metadata->resource_info.num_uniform_buffers;

    const uint64_t key = HashBytes(spv, spv_size) ^ (uint64_t)SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    if (auto cached = LookupGpuShader(key)) {
        SDL_free(metadata);
        fs.shader_data.shader = cached;
        return fs;
    }

    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = spv; info.bytecode_size = spv_size;
    info.entrypoint = "main"; info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT; info.props = 0;
    SDL_GPUShader* raw = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &info, &metadata->resource_info, 0);

    SDL_free(metadata);
    if (!raw) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create GPU fragment shader: %s", dbg_name);
        return {};
    }
    fs.shader_data.shader = RegisterGpuShader(key, raw);
    return fs;
}

// spv передаётся во владение ComputeShaderData (пайплайн строится позже)
ComputeShaderData ShaderManager::BuildComputeShader(Uint8* spv, size_t spv_size, const char* dbg_name)
{
    ComputeShaderData cs{};
    cs.spv_code = spv;
    cs.spv_size = spv_size;

    SDL_ShaderCross_ComputePipelineMetadata* metadata =
        SDL_ShaderCross_ReflectComputeSPIRV(spv, spv_size, 0);
    if (metadata) {
        cs.threadcount_x = metadata->threadcount_x;
        cs.threadcount_y = metadata->threadcount_y;
        cs.threadcount_z = metadata->threadcount_z;
        cs.num_samplers = metadata->num_samplers;
        cs.num_readonly_storage_textures = metadata->num_readonly_storage_textures;
        cs.num_readonly_storage_buffers = metadata->num_readonly_storage_buffers;
        cs.num_readwrite_storage_textures = metadata->num_readwrite_storage_textures;
        cs.num_readwrite_storage_buffers = metadata->num_readwrite_storage_buffers;
        cs.num_uniform_buffers = metadata->num_uniform_buffers;
        SDL_free(metadata);
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to reflect compute shader: %s", dbg_name);
    }
    return cs;
}