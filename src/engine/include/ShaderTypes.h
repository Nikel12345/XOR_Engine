#pragma once
#include <vector>
#include <functional>
#include <string>
#include <cstdint>
#include <SDL3/SDL_gpu.h>
#include <glm/glm.hpp>
#include "Utils.h"
#include "Aliases.h"


// Слепок группы draw'а — только по указателю в PushInput (полный тип у потребителя).
namespace RenderSnap { struct TextureDraw; }

namespace ShaderBase {
    enum VertexSemantic : Uint32 { POSITION = 0, UV = 1, NORMAL = 2, TANGENT = 3 };

    struct VertexAttr {
        VertexSemantic semantic;
        Uint32 offset;
        SDL_GPUVertexElementFormat format;
    };

    struct VertexFormat {
        std::vector<VertexAttr> attrs;
        Uint32 stride;
        const VertexAttr* Find(VertexSemantic s) const {
            for (auto& a : attrs) if (a.semantic == s) return &a;
            return nullptr;
        }
    };
    struct VertexBufferBinding {
        const VertexFormat* format;
        std::vector<VertexSemantic> pull;
    };
}

struct ShaderDefine {
    std::string name;
    std::string value;
};
using ShaderDefines = std::vector<ShaderDefine>;

enum class PushStage : uint8_t { Vertex, Fragment, Compute };

struct PushInput {
    const void*                    pass_state = nullptr;
    const RenderSnap::TextureDraw* draw       = nullptr;
};

struct PushConstantBinder {
    SDL_GPUCommandBuffer* cb = nullptr;
    PushStage stage = PushStage::Fragment;
    Uint32    uniform_slot = 0;
    uint8_t   frame = 0;

    template<typename T> void Push(const T& d) const { PushRaw(&d, sizeof(T)); }
    template<typename T> void Push(const std::vector<T>& v) const {
        PushRaw(v.data(), safe_u32(v.size() * sizeof(T)));
    }

private:
    void PushRaw(const void* data, Uint32 size) const {
        switch (stage) {
        case PushStage::Vertex:   SDL_PushGPUVertexUniformData(cb, uniform_slot, data, size);   break;
        case PushStage::Fragment: SDL_PushGPUFragmentUniformData(cb, uniform_slot, data, size); break;
        case PushStage::Compute:  SDL_PushGPUComputeUniformData(cb, uniform_slot, data, size);  break;
        }
    }
};

using PushFunc = std::function<void(const struct PushConstantBinder&, const struct PushInput&)>;

struct PushInstruction {
    PushStage stage = PushStage::Fragment;
    Uint32    uniform_slot = 0;
    PushFunc  fn;
};
using PushInstructions = std::vector<PushInstruction>;

struct ShaderPushInstruction {
    std::string program_name;
    PushStage   stage = PushStage::Fragment;
    PushFunc    fn;
};

struct DispatchSizeBinder {
    glm::uvec3 element_count{ 0, 0, 0 };
    uint8_t frame = 0;

    void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) {
        element_count = { x, y, z };
    }
};

struct RasterizerStateBiasParams {
    float depth_bias_constant_factor = 0.0f;
    float depth_bias_slope_factor = 0.0f;
    float depth_bias_clamp = 0.0f;
    bool enable_depth_bias = false;
};

struct ShaderProgramDescription
{
    SDL_GPUCullMode           cull_mode = SDL_GPU_CULLMODE_NONE;
    SDL_GPUFillMode           fill_mode = SDL_GPU_FILLMODE_FILL;
    RasterizerStateBiasParams rasterizer_bias;
    bool                      depth_test = true;
    bool                      depth_write = true;
    SDL_GPUCompareOp          depth_compare_op = SDL_GPU_COMPAREOP_LESS;
    bool                      stencil_test = false;
    bool                      color_blend = false;
    SDL_GPUPrimitiveType primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;


    ShaderProgramDescription* BehavesAsShadowCaster();
    ShaderProgramDescription* BehavesAsOpaqueGeometry();
    ShaderProgramDescription* BehavesAsTransparentGeometry();
    ShaderProgramDescription* BehavesAsDepthPrepass();
    ShaderProgramDescription* BehavesAsFullscreenEffect();
    ShaderProgramDescription* BehavesAsUIOverlay();

    ShaderProgramDescription* WithBlending() { color_blend = true;  return this; }
    ShaderProgramDescription* WithoutBlending() { color_blend = false; return this; }
    ShaderProgramDescription* IgnoresDepth() { depth_test = false; depth_write = false; return this; }
    ShaderProgramDescription* ReadsDepthOnly() { depth_test = true;  depth_write = false; return this; }
    ShaderProgramDescription* WritesDepth() { depth_test = true;  depth_write = true;  return this; }
    ShaderProgramDescription* WithDepthCompare(SDL_GPUCompareOp op) { depth_compare_op = op; return this; }
    ShaderProgramDescription* CullsBackFaces() { cull_mode = SDL_GPU_CULLMODE_BACK;  return this; }
    ShaderProgramDescription* CullsFrontFaces() { cull_mode = SDL_GPU_CULLMODE_FRONT; return this; }
    ShaderProgramDescription* DoesNotCull() { cull_mode = SDL_GPU_CULLMODE_NONE;  return this; }
    ShaderProgramDescription* WithDepthBias(RasterizerStateBiasParams b) { rasterizer_bias = b; return this; }
    ShaderProgramDescription* Wireframe() { fill_mode = SDL_GPU_FILLMODE_LINE; return this; }
    ShaderProgramDescription* Solid() { fill_mode = SDL_GPU_FILLMODE_FILL; return this; }
    ShaderProgramDescription* AsLineList() { primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST; return this; }
    ShaderProgramDescription* AsPointList() { primitive_type = SDL_GPU_PRIMITIVETYPE_POINTLIST; return this; }
};

enum class TextureSlotRole {
    Albedo,
    Normal,
    ORM,
    Emissive,
    MetallicRoughness = ORM,

    // Роли без семантики: движок биндит по ним хэндл, а сэмплеры под них объявляет сам пролог.
    // Разрыв в нумерации — резерв под новые well-known роли: номер уезжает в сцену ЧИСЛОМ
    Custom0 = 1000,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Custom5,
    Custom6,
    Custom7,
};

inline constexpr uint32_t MAX_SLOTS = 12;
inline constexpr uint32_t MAX_VARIATIVE_SLOTS = 4;
inline constexpr uint32_t MAX_UVL_BLOCKS = 32;

struct ComputeRWTextureBindingParametr {
    std::string texture_atlas = "";
    Uint32 mip_level = 0;
    Uint32 layer = 0;
    bool need_simultaneous = false;
};

// Хранимый вид того же биндинга: параметр приезжает с ИМЕНЕМ атласа (его печатает вызывающий),
// программа держит id ячейки реестра.
struct ComputeRWTextureBinding {
    AtlasId texture_atlas;
    Uint32 mip_level = 0;
    Uint32 layer = 0;
    bool need_simultaneous = false;
};
