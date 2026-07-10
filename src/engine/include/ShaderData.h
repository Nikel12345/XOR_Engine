#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <functional>
#include <memory>
#include <SDL3/SDL_gpu.h>
#include "ShaderTypes.h"   // лёгкая половина: enum'ы/вершинные форматы/spd/биндеры
#include "Aliases.h"       // BufferDataName — имя storage-буфера в ссылках sp

// ТЯЖЁЛАЯ половина: живые структуры шейдер-данных и программ (GPU-хэндлы, std::function,
// рефлексия). Правится часто (редактор шейдеров) — потребители подключают её ТОЛЬКО если
// реально лезут внутрь sp/csp/шейдер-данных (обычно через ShaderManager.h/GpuTaskContext.h).
// Материалам/вершинам/UI-командам хватает ShaderTypes.h — не тяни этот файл в заголовки-хабы.

struct BufferData;
struct RenderPassStep;
struct ComputePassStep;
struct TextureAtlas;

namespace ShaderBase {
    struct ShaderData
    {
        // Владеющий хэндл GPU-шейдера. Шарится копированием ShaderData (vs reuse между sp) и
        // дедупом в ShaderManager → refcount = число владельцев, релиз при 0 (делитер задаёт
        // ShaderManager и гасит его после своей смерти). Пайплайн берёт сырой через .get().
        std::shared_ptr<SDL_GPUShader> shader;
        size_t shader_size = 0;
        Uint8* shader_code = nullptr;
        Uint32 num_uniform_buffers = 0;   // из рефлексии; гейт пуша params (объявил ли шейдер слот MaterialBlock)
    };
}

struct VertexShaderData {
    ShaderBase::ShaderData shader_data;
    std::vector<SDL_GPUVertexAttribute> attributes;
    std::vector<SDL_GPUVertexBufferDescription> vbs;
    // Исходник + раскладка буферов — чтобы редактор мог ПЕРЕКОМПИЛИРОВАТЬ vs из (возможно
    // изменённого) пути, не зная контекста создания. bindings копируют статику (buffer/format —
    // указатели на глобальные имена/форматы), поэтому переживают программу. Пусто у SPV/дефолтных.
    std::string source_path;
    std::vector<ShaderBase::VertexBufferBinding> bindings;
    bool dont_save = false;   // движковый дефолт (_fallback_vs) — в shaders.json не пишется
};

struct FragmentShaderData {
    ShaderBase::ShaderData shader_data;
    std::string source_path;   // исходник fs (для перекомпиляции из редактора; см. VertexShaderData)
    bool dont_save = false;    // см. VertexShaderData::dont_save
};

struct ComputeShaderData {
	Uint8* spv_code = nullptr;
	size_t spv_size = 0;
    std::string source_path;   // исходник cs (для перекомпиляции/сериализации; см. VertexShaderData)
    Uint32 threadcount_x = 1;
    Uint32 threadcount_y = 1;
    Uint32 threadcount_z = 1;
    Uint32 num_samplers = 0;
    Uint32 num_readonly_storage_textures = 0;
    Uint32 num_readonly_storage_buffers = 0;
    Uint32 num_readwrite_storage_textures = 0;
    Uint32 num_readwrite_storage_buffers = 0;
    Uint32 num_uniform_buffers = 0;
    bool dont_save = false;    // см. VertexShaderData::dont_save
};

struct ShaderProgram {
    // vs/fs — ССЫЛКИ ПО ИМЕНИ на именованные VertexShaderData/FragmentShaderData в реестрах
    // ShaderManager (не по значению): один GPU-шейдер шарится между sp, правка sp не пересобирает
    // шейдер, удаление sp не трогает шейдер. Резолв — на сборке пайплайна/батча (как текстуры материала).
    std::string vs_name;
    // Storage-буферы стадий — ССЫЛКИ ПО ИМЕНИ (BufferDataName = ключ реестра BufferManager, как
    // vs_name/fs_name), порядок = слоты бинда. Резолв в BufferData* — на сборке батча через
    // GetBufferData (по идентичности указателя-ключа, как и все прочие обращения к буферам).
    std::vector<BufferDataName> vertex_shader_buffer_names;

    std::string fs_name;
    std::vector<BufferDataName> fragment_shader_buffer_names;

    // Expected texture types (by role) for this shader. For example, if the shader has a
    // uniform sampler2D u_albedoTexture, then required_slots will contain TextureSlotRole::Albedo.
    std::vector<TextureSlotRole> required_slots;

    std::function<void(const PushConstantBinder&, const void*)> push_func;
    template<typename T, typename Fn> void BindPushConstants(Fn&& fn) {
        push_func = [fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const void* raw) {
            fn(b, *static_cast<const T*>(raw));
        };
    }
	ShaderProgramDescription spd;   // ПО ЗНАЧЕНИЮ: параметры пайплайна живут в самом sp (не в словаре)
    RenderPassStep* associated_render_pass = nullptr;
    bool dont_save = false;   // движковый дефолт (_FallbackShader) — в shaders.json не пишется

};


struct ComputeShaderProgram {
    struct ComputeRWTextureBinding {
        TextureAtlas* texture_atlas = nullptr;
        Uint32 mip_level = 0;
        Uint32 layer = 0;
    };
    // Форма создания «по именам» — теперь топ-левел тип в ShaderTypes.h (сигнатуры фасадов
    // не требуют полного ComputeShaderProgram); алиас сохраняет старое вложенное написание.
    using ComputeRWTextureBindingParametr = ::ComputeRWTextureBindingParametr;
    std::string cs_name;   // ссылка по имени на ComputeShaderData в реестре ShaderManager (см. ShaderProgram)
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
