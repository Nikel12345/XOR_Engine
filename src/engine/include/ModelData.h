#pragma once
#include <vector>
#include <string>
#include <functional>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
// Диапазон в буферах пула, В ЭЛЕМЕНТАХ.
struct GeometryRange {
    uint32_t first = 0;
    uint32_t count = 0;
};

// Вершины заполняются БАЙТАМИ в раскладке своего пула.
using ModelGeneratorFn = std::function<void(std::vector<std::byte>&, std::vector<Uint32>&)>;

template<class V>
inline void WriteVertices(std::vector<std::byte>& out, const std::vector<V>& src)
{
    const size_t base = out.size();
    out.resize(base + src.size() * sizeof(V));
    if (!src.empty()) std::memcpy(out.data() + base, src.data(), src.size() * sizeof(V));
}

// Ступень лесенки экранных размеров: 0 = граница не задана, иначе порог = 0.5 * 2^(L-1) px.
struct SubMeshSpan {
    uint8_t lod_min = 0;
    uint8_t lod_max = 0;
};

struct SubMeshData {
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;
	uint32_t material_index = 0;
    glm::vec4 sphere;
    glm::vec3 aabb_center = glm::vec3(0.0f);
    glm::vec3 aabb_half   = glm::vec3(0.0f);
    SubMeshSpan screen_size_span;
};

inline constexpr uint32_t kCmdIndexMask = 0x00FFFFFFu;
inline uint32_t MakeEntityToCmdWord(uint32_t cmd_index, SubMeshSpan span) {
    assert(cmd_index <= kCmdIndexMask);
    return (cmd_index & kCmdIndexMask)
         | (static_cast<uint32_t>(span.lod_min & 0xFu) << 24)
         | (static_cast<uint32_t>(span.lod_max & 0xFu) << 28);
}

// L/R = X min/max, B/T = Y min/max (Bottom/Top), B/F = Z min/max (Back/Front).
enum class AnchorShift { Keep, Center, LBB, RBB, LTB, RTB, LBF, RBF, LTF, RTF };

struct ModelData {
    std::vector<SubMeshData> submeshes;
    AnchorShift anchor = AnchorShift::Keep;

    // Пусто = дефолтный пул.
    std::string pool_name;

    // При placed == false отсчитывается от начала стейджинга, дальше — от начала буфера.
    GeometryRange vertex_range;
    GeometryRange index_range;
    bool          placed = false;

    // Пусты у процедурных моделей: из файла они не пересоздаются.
    std::string model_path;
    std::string index_path;
    bool dont_save = false;
};