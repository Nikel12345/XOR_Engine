#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <SDL3/SDL.h>
#include "ShaderTypes.h"


struct PosUVNormal {
    float x, y, z;       // позиция
    float u, v;          // UV
    float nx, ny, nz;    // normal
    float tx, ty, tz;  // tangent
};
struct PosOnly { float x, y, z; };

inline const ShaderBase::VertexFormat FMT_PosUVNormal = {   // inline: one entity per program (pointer identity!)
    {
        { ShaderBase::POSITION, offsetof(PosUVNormal, x),  SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
        { ShaderBase::UV,       offsetof(PosUVNormal, u),  SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2 },
        { ShaderBase::NORMAL,   offsetof(PosUVNormal, nx), SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
        { ShaderBase::TANGENT,  offsetof(PosUVNormal, tx), SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
    },
    sizeof(PosUVNormal)
};

inline const ShaderBase::VertexFormat FMT_Pos = {
    { { ShaderBase::POSITION, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 } },
    sizeof(PosOnly)
};


//std::vector<PosUV> MakeCubeVertices();
std::vector<PosUVNormal> MakeCubeVerticesNorm();

//std::vector<PosUV> MakeSphereVertices();
std::vector<Uint16> MakeSphereIndices();


extern const std::vector<Uint16> IndicesCube;
extern const std::vector<Uint16> IndicesSquare;