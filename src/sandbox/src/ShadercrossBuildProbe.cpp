#include "PCH.h"
#include <SDL3_shadercross/SDL_shadercross.h>
#include <windows.h>

/*
    Зонд: shadercross, собранный из исходников (external/SDL3_shadercross), компилирует HLSL и
    рефлексирует результат, а dxcompiler.dll берётся из каталога exe, а не по PATH.

    Второе не формальность: на машине разработчика в PATH лежит Vulkan SDK со своим DXC другой
    версии, и до вендоринга именно он молча компилировал все шейдеры.
*/

static const char* kCompute =
    "RWStructuredBuffer<uint> buf : register(u0, space1);\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) { buf[id.x] = id.x; }\n";

int main(int, char**)
{
    if (!SDL_ShaderCross_Init()) { SDL_Log("ShaderCross_Init failed: %s", SDL_GetError()); return 1; }

    SDL_ShaderCross_HLSL_Info info{};
    info.source = kCompute;
    info.entrypoint = "main";
    info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    size_t size = 0;
    void* spv = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &size);
    if (!spv) { SDL_Log("compile FAILED: %s", SDL_GetError()); return 1; }
    const Uint32 spirv_version = reinterpret_cast<const Uint32*>(spv)[1];
    SDL_Log("compile ok: %zu bytes, SPIR-V %u.%u", size, (spirv_version >> 16) & 0xff, (spirv_version >> 8) & 0xff);

    SDL_ShaderCross_ComputePipelineMetadata* meta = SDL_ShaderCross_ReflectComputeSPIRV(static_cast<Uint8*>(spv), size, 0);
    if (meta) {
        SDL_Log("reflect ok: threads %u x %u x %u, rw buffers %u", meta->threadcount_x, meta->threadcount_y,
            meta->threadcount_z, meta->num_readwrite_storage_buffers);
        SDL_free(meta);
    } else {
        SDL_Log("reflect FAILED: %s", SDL_GetError());
    }
    SDL_free(spv);

    char path[MAX_PATH] = {};
    if (HMODULE dxc = GetModuleHandleA("dxcompiler.dll"))
        GetModuleFileNameA(dxc, path, MAX_PATH);
    SDL_Log("dxcompiler.dll loaded from: %s", path[0] ? path : "(not loaded)");

    SDL_ShaderCross_Quit();
    return 0;
}
