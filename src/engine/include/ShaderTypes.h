#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <initializer_list>
#include <SDL3/SDL_gpu.h>
#include <glm/glm.hpp>

// ЛЁГКАЯ половина бывшего ShaderData.h: enum'ы, вершинные форматы, описание пайплайна (spd),
// биндеры пуш-констант. Это стабильные типы, которые нужны материалам (TextureSlotRole),
// вершинам (VertexFormat), UI-командам (spd) — БЕЗ тяжёлых структур шейдеров
// (ShaderProgram/ShaderData/...), которые правятся часто и живут в ShaderData.h.
// Правка ЭТОГО файла пересобирает почти весь движок — сюда только устоявшееся.

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
        // Имени буфера тут нет: слот позиционный (порядок биндингов), а какой реальный
        // GPU-буфер идёт в слот — решает бинд-шаг рендера (BufferManager::BindGPUVertexBuffer).
        const VertexFormat* format;
        std::vector<VertexSemantic> pull;
    };
}

// using namespace ShaderBase из заголовка УБРАН намеренно: директива в хедере сливала
// POSITION/UV/VertexFormat в глобал каждому включившему. Заголовки квалифицируют
// ShaderBase:: явно; cpp при желании пишет using namespace ShaderBase; у себя.

// Дефайн препроцессора HLSL, подкидываемый компилятору на сборке (DXC -D NAME=VALUE):
// шейдер объявляет константу через #ifndef/#define, а вызов Create*Shader её переопределяет.
// value == nullptr — дефайн без значения (под #ifdef).
// Строки НЕ копируются: они должны пережить вызов Create*Shader (на практике — литералы).
// Имя и значение входят в ключ кэша .spv (ShaderManager::LoadOrCompileSPIRV) — без этого два
// варианта одного .hlsl делили бы один файл кэша, и второй молча получил бы чужой байткод.
struct ShaderDefine {
    const char* name;
    const char* value = nullptr;
};
using ShaderDefines = std::initializer_list<ShaderDefine>;

struct PushConstantBinder {
    SDL_GPUCommandBuffer* cb;
    // Слот кадра (pass_frame/render_frame) — ключ пер-слотовых слепков: push-лямбды берут
    // данные ТОЛЬКО через Ask*(binder.slot), живые ECS/дерево батчей из них запрещены.
    uint8_t slot = 0;
    mutable Uint32 vert_count = 0;
    mutable Uint32 frag_count = 0;

    // compute — слот задаёт вызывающий явно
    template<typename T> void Push(Uint32 slot, const T& d) const {
        SDL_PushGPUComputeUniformData(cb, slot, &d, sizeof(T));
    }
    // graphics — авто-слот инкрементом (mutable: лямбда остаётся const, как у compute)
    template<typename T> void PushVertex(const T& d) const {
        SDL_PushGPUVertexUniformData(cb, vert_count++, &d, sizeof(T));
    }
    template<typename T> void PushFragment(const T& d) const {
        SDL_PushGPUFragmentUniformData(cb, frag_count++, &d, sizeof(T));
    }
};

struct DispatchSizeBinder {
    glm::uvec3 element_count{ 0, 0, 0 };
    // Слот кадра — тот же контракт, что у PushConstantBinder::slot (см. выше).
    uint8_t slot = 0;

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
    // Компаратор depth-теста (читается пайплайном при depth_test). LESS — прежний захардкоженный
    // дефолт; LESS_OR_EQUAL нужен приёмам «глубина ровно на клире» (скайбокс: z=w → 1.0).
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
};

enum class TextureSlotRole {
    // Well-known PBR-роли: движок знает их семантику (colorspace, встроенные хелперы
    // SampleAlbedo/computeNormal/SampleORM). Порядок здесь не задаёт бинды — их порядок
    // диктует ShaderProgram::required_slots (он же → textures[i] в прологе).
    Albedo,
    Normal,
    ORM,                     // упаковка: R=AO, G=Roughness, B=Metallic (одна текстура, один UVL)
    Emissive,
    MetallicRoughness = ORM, // back-compat алиас: тот же слот, что ORM (старое имя)

    // Generic-слоты для пользовательских прологов: движок просто биндит хэндл по роли,
    // никакой семантики. Кастомный surface.hlsl сам объявляет сэмплеры/textures[] под них.
    Custom0 = 1000,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Custom5,
    Custom6,
    Custom7,
};

// Параметры RW-текстуры компьют-программы «по именам» (форма создания). Топ-левел, а не
// вложен в ComputeShaderProgram: сигнатуры фасадов (EngineContext/GpuTaskContext) называют
// его без полного определения ComputeShaderProgram; внутри того остаётся алиас со старым
// вложенным именем — все прежние написания продолжают работать.
struct ComputeRWTextureBindingParametr {
    std::string texture_atlas = "";
    Uint32 mip_level = 0;
    Uint32 layer = 0;
    // true → шейдер читает СОСЕДНИЕ тексели этой же текстуры, пока другие потоки диспатча их пишут
    // (bloom_up: tent-фильтр апсемпла делает RMW по одному уровню). Тогда SDL нужен
    // COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE — это НЕ то же самое, что READ|WRITE (SDL_gpu.h:828).
    //
    // Автовывести нельзя: это факт о ТЕЛЕ шейдера, а не о форме бинда. bloom_up и bloom_composite
    // регистрируются идентично (по одному rw-биндингу, ro пуст, сэмплер — на ЧУЖОЙ атлас), но
    // composite трогает только СВОЙ тексель (`dst[id] = f(dst[id], src[id])`) — гонки нет, флаг не
    // нужен; а up читает соседей — нужен. Различие видно только автору шейдера, поэтому — ручной тег.
    bool need_simultaneous = false;
};
