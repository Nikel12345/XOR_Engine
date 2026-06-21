#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <functional>
#include <memory>
#include <SDL3/SDL_gpu.h>
#include <glm/glm.hpp>   // DispatchSizeBinder использует glm::uvec3 (раньше приходил из PCH)

struct BufferData;
struct RenderPassStep;
struct ComputePassStep;
struct TextureAtlas;

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
        const char* buffer;
        const VertexFormat* format;
        std::vector<VertexSemantic> pull;
    };
    struct ShaderData
    {
        // Владеющий хэндл GPU-шейдера. Шарится копированием ShaderData (vs reuse между sp) и
        // дедупом в ShaderManager → refcount = число владельцев, релиз при 0 (делитер задаёт
        // ShaderManager и гасит его после своей смерти). Пайплайн берёт сырой через .get().
        std::shared_ptr<SDL_GPUShader> shader;
        size_t shader_size = 0;
        Uint8* shader_code = nullptr;
    };
}

using namespace ShaderBase;

struct PushConstantBinder {
    SDL_GPUCommandBuffer* cb;
    mutable Uint32 vert_count = 0;
    mutable Uint32 frag_count = 0;

    // compute � ��� ����, ����� ����
    template<typename T> void Push(Uint32 slot, const T& d) const {
        SDL_PushGPUComputeUniformData(cb, slot, &d, sizeof(T));
    }
    // graphics � ����-���� + ���� (mutable, ����� ������ �������� const, ��� � compute)
    template<typename T> void PushVertex(const T& d) const {
        SDL_PushGPUVertexUniformData(cb, vert_count++, &d, sizeof(T));
    }
    template<typename T> void PushFragment(const T& d) const {
        SDL_PushGPUFragmentUniformData(cb, frag_count++, &d, sizeof(T));
    }
};

struct DispatchSizeBinder {
    glm::uvec3 element_count{ 0, 0, 0 };

    void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) {
        element_count = { x, y, z };
    }
};

struct VertexShaderData {
    ShaderData shader_data;
    std::vector<SDL_GPUVertexAttribute> attributes;
    std::vector<SDL_GPUVertexBufferDescription> vbs;
};

struct FragmentShaderData {
    ShaderData shader_data;
};

struct ComputeShaderData {
	Uint8* spv_code = nullptr;
	size_t spv_size = 0;
    Uint32 threadcount_x = 1;
    Uint32 threadcount_y = 1;
    Uint32 threadcount_z = 1;
    Uint32 num_samplers = 0;
    Uint32 num_readonly_storage_textures = 0;
    Uint32 num_readonly_storage_buffers = 0;
    Uint32 num_readwrite_storage_textures = 0;
    Uint32 num_readwrite_storage_buffers = 0;
    Uint32 num_uniform_buffers = 0;
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
    ShaderProgramDescription* CullsBackFaces() { cull_mode = SDL_GPU_CULLMODE_BACK;  return this; }
    ShaderProgramDescription* CullsFrontFaces() { cull_mode = SDL_GPU_CULLMODE_FRONT; return this; }
    ShaderProgramDescription* DoesNotCull() { cull_mode = SDL_GPU_CULLMODE_NONE;  return this; }
    ShaderProgramDescription* WithDepthBias(RasterizerStateBiasParams b) { rasterizer_bias = b; return this; }
    ShaderProgramDescription* Wireframe() { fill_mode = SDL_GPU_FILLMODE_LINE; return this; }
    ShaderProgramDescription* Solid() { fill_mode = SDL_GPU_FILLMODE_FILL; return this; }
    ShaderProgramDescription* AsLineList() { primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST; return this; }
};

enum class TextureSlotRole {
    Albedo,
    Normal,
    Emissive,
	MetallicRoughness,
};

struct ShaderProgram {
    VertexShaderData vs;
    std::vector<BufferData*> vertex_shader_buffers;

    FragmentShaderData fs;
    std::vector<BufferData*> fragment_shader_buffers;

    // ��������� ���� (�� ����������) ������� ��� ����� �������. ��������, ���� � ������� ���� uniform sampler2D u_albedoTexture, �� � required_slots ����� TextureSlotRole::Albedo.
	// Expected texture types (by role) for this shader. For example, if the shader has a uniform sampler2D u_albedoTexture, then required_slots will contain TextureSlotRole::Albedo.
    std::vector<TextureSlotRole> required_slots;

    std::function<void(const PushConstantBinder&, const void*)> push_func;
    template<typename T, typename Fn> void BindPushConstants(Fn&& fn) {
        push_func = [fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const void* raw) {
            fn(b, *static_cast<const T*>(raw));
        };
    }
	ShaderProgramDescription* spd;
    RenderPassStep* associated_render_pass = nullptr;

};


struct ComputeShaderProgram {
    struct ComputeRWTextureBinding {
        TextureAtlas* texture_atlas = nullptr;
        Uint32 mip_level = 0;
        Uint32 layer = 0;
    };
    struct ComputeRWTextureBindingParametr {
        std::string texture_atlas = "";
        Uint32 mip_level = 0;
        Uint32 layer = 0;
    };
    ComputeShaderData cs;
    std::vector<BufferData*> rw_storage_buffers;
    std::vector<BufferData*> ro_storage_buffers;
    std::vector<ComputeRWTextureBinding> rw_storage_textures;
    std::vector<TextureAtlas*> ro_storage_textures;
    std::vector<TextureAtlas*> texture_samplers;
    std::string debug_name;

    std::function<void(const PushConstantBinder&, const void*)> push_func = nullptr;
    template<typename T, typename Fn>
    void BindPushConstants(Fn&& fn) {
        push_func = [fn = std::forward<Fn>(fn)](const PushConstantBinder& binder, const void* raw) {
            fn(binder, *static_cast<const T*>(raw));
        };
    }

    std::function<void(DispatchSizeBinder&, const void*)> dispatch_func = nullptr;
    template<typename T, typename Fn>
    void BindDispatch(Fn&& fn) {
        dispatch_func = [fn = std::forward<Fn>(fn)](DispatchSizeBinder& binder, const void* raw) {
            fn(binder, *static_cast<const T*>(raw));
        };
    }

    ComputePassStep* associated_compute_pass = nullptr;
    BufferData* indirect_buffer = nullptr;
};