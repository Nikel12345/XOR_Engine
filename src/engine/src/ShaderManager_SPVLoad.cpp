#include "PCH.h"
#include "ShaderManager.h"

using namespace ShaderBase;


VertexShaderData ShaderManager::CreateVertexShaderFromSPV(const char* path, std::initializer_list<VertexBufferBinding> bindings)
{
    size_t n = 0;
    Uint8* spv = (Uint8*)SDL_LoadFile(path, &n);
    if (!spv) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load SPV: %s", path); return {}; }
    VertexShaderData vs = BuildVertexShader(spv, n, path, std::vector<VertexBufferBinding>(bindings));
    SDL_free(spv);
    return vs;
}

FragmentShaderData ShaderManager::CreateFragmentShaderFromSPV(const char* path)
{
    size_t n = 0;
    Uint8* spv = (Uint8*)SDL_LoadFile(path, &n);
    if (!spv) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load SPV: %s", path); return {}; }
    FragmentShaderData fs = BuildFragmentShader(spv, n, path);
    SDL_free(spv);
    return fs;
}

ComputeShaderData ShaderManager::CreateComputeShaderFromSPV(const char* path)
{
    size_t n = 0;
    Uint8* spv = (Uint8*)SDL_LoadFile(path, &n);
    if (!spv) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load SPV: %s", path); return {}; }
    return BuildComputeShader(spv, n, path);   // �������� spv ������ � cs
}
