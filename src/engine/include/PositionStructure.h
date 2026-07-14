#pragma once
#include <vector>
#include <string_view>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <SDL3/SDL.h>
#include "ShaderTypes.h"


// ФОРМАТ ЗАГРУЗКИ моделей (CPU-стейджинг, .bin-файлы, генераторы CreateModel): интерлив.
// На GPU в таком виде БОЛЬШЕ НЕ ЖИВЁТ — заливка расщепляет его на стримы пула (см. ниже).
struct PosUVNormal {
    float x, y, z;       // позиция
    float u, v;          // UV
    float nx, ny, nz;    // normal
    float tx, ty, tz;    // tangent
};
struct PosOnly { float x, y, z; };

// ═══ Пул геометрии PosUVNorm: вершинные данные моделей, разложенные ПО СТРИМАМ ═══
// Стрим = ОТДЕЛЬНЫЙ GPU-буфер со своей плотной раскладкой (не регион общего буфера: регионы не
// могут расти независимо, а вся машинерия EnsureBufferCapacity/RESIZE_AND_COPY — пер-буферная).
// Все стримы пула растут В НОГУ: вершина №N существует в каждом (vertex_offset индирект-команды
// один на все слоты — это требование API, не выбор). Канон продвижения — элементный счётчик
// ModelManager; байтовые append-точки стримов = счётчик × stride стрима.
//
// Шейдер объявляет ПОТРЕБЛЯЕМЫЕ стримы перечислением ИМЁН БУФЕРОВ по слотам
// (CreateVertexShader) — теням достаточно Pos-стрима: 12 байт/вершину вместо 44 интерлива.
// Раскладка слота (страйд/атрибуты) резолвится из таблицы пула ниже; локации атрибутов = семантики
// ([[vk::location]]-конвенция), поэтому HLSL от разбивки не меняется.
//
// Normal и Tangent объединены в один стрим НАМЕРЕННО: они потребляются строго вместе
// (нормал-маппингу нужны оба, теням — ни один). UV отдельно: будущий потребитель Pos+UV
// без нормалей — alpha-tested тени.
namespace GeometryStreams {
    inline constexpr const char* VERTEX_POS_BUFFER     = "_VertexPosBuffer";
    inline constexpr const char* VERTEX_UV_BUFFER      = "_VertexUVBuffer";
    inline constexpr const char* VERTEX_NORMTAN_BUFFER = "_VertexNormTanBuffer";
}

// Плотные раскладки стримов. offset'ы — ВНУТРИ стрима (Pos: xyz с нуля; NormTan: normal, за ним tangent).
inline const ShaderBase::VertexFormat FMT_PosStream = {   // inline: one entity per program (pointer identity!)
    { { ShaderBase::POSITION, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 } },
    12
};
inline const ShaderBase::VertexFormat FMT_UVStream = {
    { { ShaderBase::UV, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2 } },
    8
};
inline const ShaderBase::VertexFormat FMT_NormTanStream = {
    {
        { ShaderBase::NORMAL,  0,  SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
        { ShaderBase::TANGENT, 12, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 },
    },
    24
};

namespace PosUVNormPool {
    struct Stream {
        const char* buffer_name;
        const ShaderBase::VertexFormat* format;
        uint32_t src_offset;   // офсет группы в PosUVNormal (формат ЗАГРУЗКИ) — для расщепления при заливке
    };
    // Порядок объявления = канонический порядок слотов (Pos, UV, NormTan).
    inline constexpr Stream streams[] = {
        { GeometryStreams::VERTEX_POS_BUFFER,     &FMT_PosStream,     offsetof(PosUVNormal, x)  },
        { GeometryStreams::VERTEX_UV_BUFFER,      &FMT_UVStream,      offsetof(PosUVNormal, u)  },
        { GeometryStreams::VERTEX_NORMTAN_BUFFER, &FMT_NormTanStream, offsetof(PosUVNormal, nx) },
    };
    inline constexpr size_t stream_count = sizeof(streams) / sizeof(streams[0]);

    inline const Stream* FindStream(std::string_view buffer_name) {
        for (const Stream& s : streams)
            if (buffer_name == s.buffer_name) return &s;
        return nullptr;
    }

    // Семантики (язык манифеста shaders.json "pull" и UI-редактора) → имена стрим-буферов в
    // каноническом порядке, без дублей (NORMAL и TANGENT сходятся в один стрим).
    inline std::vector<const char*> StreamsForSemantics(const std::vector<ShaderBase::VertexSemantic>& pull) {
        std::vector<const char*> out;
        for (const Stream& s : streams) {
            bool used = false;
            for (ShaderBase::VertexSemantic sem : pull)
                if (s.format->Find(sem)) { used = true; break; }
            if (used) out.push_back(s.buffer_name);
        }
        return out;
    }
}


//std::vector<PosUV> MakeCubeVertices();
std::vector<PosUVNormal> MakeCubeVerticesNorm();

//std::vector<PosUV> MakeSphereVertices();
std::vector<Uint16> MakeSphereIndices();


extern const std::vector<Uint16> IndicesCube;
extern const std::vector<Uint16> IndicesSquare;