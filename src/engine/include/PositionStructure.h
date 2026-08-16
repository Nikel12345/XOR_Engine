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


// ФОРМАТ ЗАГРУЗКИ моделей (CPU-стейджинг, .bin-файлы, генераторы CreateModel): интерлив.
// На GPU в таком виде НЕ ЖИВЁТ — заливка расщепляет его на стримы пула (см. PosUVNormLayout).
struct PosUVNormal {
    float x, y, z;       // позиция
    float u, v;          // UV
    float nx, ny, nz;    // normal
    float tx, ty, tz;    // tangent
};
struct PosOnly { float x, y, z; };

// Имя пула движковой раскладки: язык манифестов (models.json/shaders.json), ключ реестра пулов.
inline constexpr const char* POS_UV_NORM_POOL = "PosUVNorm";

// Разбивка PosUVNormal на стримы — аргумент CreateGeometryPool (тот сам заведёт буферы, сгенерит
// им имена и зарегистрирует инструкции заливки).
//
// Normal и Tangent объединены в ОДИН стрим намеренно: они потребляются строго вместе
// (нормал-маппингу нужны оба, теням — ни один). UV отдельно ради будущего потребителя Pos+UV без
// нормалей — alpha-tested тени. Функция, а не глобальная константа: возвращает vector, а тот не
// должен зависеть от порядка статической инициализации.
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
