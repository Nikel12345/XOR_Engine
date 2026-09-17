#pragma once
#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <SDL3/SDL.h>
#include "ShaderTypes.h"
#include "GeometryPool.h"
#include "Utils.h"

struct PosUVNormal {
    float x, y, z;       // позиция
    float u, v;          // UV
    float nx, ny, nz;    // normal
    float tx, ty, tz;    // tangent
};
struct PosOnly { float x, y, z; };

inline constexpr const char* POS_UV_NORM_POOL = "PosUVNorm";


// Normal и Tangent объединены в ОДИН стрим намеренно: они потребляются строго вместе
inline std::vector<GeometryPool::StreamDesc> PosUVNormLayout()
{
    using namespace ShaderBase;
    return {
        { { { POSITION, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 } },
          12, safe_u32(offsetof(PosUVNormal, x)) },

        { { { UV, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2 } },
          8, safe_u32(offsetof(PosUVNormal, u)) },

        { { { NORMAL,  0,  SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
            { TANGENT, 12, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 } },
          24, safe_u32(offsetof(PosUVNormal, nx)) },
    };
}
