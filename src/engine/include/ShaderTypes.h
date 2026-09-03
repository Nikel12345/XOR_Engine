#pragma once
#include <vector>
#include <functional>
#include <string>
#include <cstdint>
#include <SDL3/SDL_gpu.h>
#include <glm/glm.hpp>
#include "Utils.h"   // safe_u32 в пуше массива блоков

// ЛЁГКАЯ половина бывшего ShaderData.h: enum'ы, вершинные форматы, описание пайплайна (spd),
// биндеры пуш-констант. Это стабильные типы, которые нужны материалам (TextureSlotRole),
// вершинам (VertexFormat), UI-командам (spd) — БЕЗ тяжёлых структур шейдеров
// (ShaderProgram/ShaderData/...), которые правятся часто и живут в ShaderData.h.
// Правка ЭТОГО файла пересобирает почти весь движок — сюда только устоявшееся.

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
// Пустое value — дефайн без значения (компилятор считает его равным 1).
// Строки ВЛАДЕЮЩИЕ: набор переживает вызов, потому что он часть рецепта пересборки шейдера —
// лежит в *ShaderData рядом с source_path, едет в shaders.json и в команды редактора.
// Имя и значение входят в ключ кэша .spv (ShaderManager::LoadOrCompileSPIRV) — без этого два
// варианта одного .hlsl делили бы один файл кэша, и второй молча получил бы чужой байткод.
// vector, а не initializer_list: список строится в РАНТАЙМЕ (загрузка сцены, форма редактора),
// а фигурные скобки на вызове он принимает так же.
struct ShaderDefine {
    std::string name;
    std::string value;
};
using ShaderDefines = std::vector<ShaderDefine>;

// Стадия, в чьи uniform-слоты пишет инструкция. Слоты нумеруются НЕЗАВИСИМО по стадиям, поэтому
// порядок вершинных и фрагментных инструкций между собой не значит ничего.
enum class PushStage : uint8_t { Vertex, Fragment, Compute };

// ВХОД инструкции — данные, из которых она собирает свой блок. Не «контекст»: имя ctx в движке
// занято EngineContext, и путать эти две вещи не стоит. Источника три, функтор берёт нужный:
//   pass_state — состояние прохода (rp.state); его ТИП знает только сам проход, отсюда void*;
//   draw       — текущая группа текстур из слепка батча (uvl, params материала, раскладка);
//   frame      — кадровый слот, ключ пер-слотовых слепков (Ask*(frame)).
// Живые ECS/дерево батчей функтору по-прежнему запрещены: только слепки.
struct PushInput {
    const void*                    pass_state = nullptr;
    const RenderSnap::TextureDraw* draw       = nullptr;
    uint8_t                        frame      = 0;
};

// Приёмник РОВНО ОДНОГО блока: стадию и слот назначил движок (позиция инструкции в списке своей
// стадии), функтору остаётся отдать данные. Слот намеренно НЕ выбирается функтором: инструкция,
// пушнувшая лишний блок, сдвинула бы регистры всем следующим — а такое видно только глазами по
// картинке. Один пуш = один cbuffer, и это структурно, а не по договорённости.
struct PushConstantBinder {
    SDL_GPUCommandBuffer* cb = nullptr;
    PushStage stage = PushStage::Fragment;
    Uint32    uniform_slot = 0;
    uint8_t   frame = 0;   // тот же кадровый слот, что в PushInput — для краткости лямбд

    // Один блок: размер берётся из типа.
    template<typename T> void Push(const T& d) const { PushRaw(&d, sizeof(T)); }
    // МАССИВ блоков (таблица UVL, блоб params материала): размер берётся из самого контейнера.
    // Перегрузка, а не «сырой» пуш с void*: тип данных известен и в этом случае, мерить их
    // руками на каждом вызове незачем.
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

// Одна push-инструкция программы: собирает свой блок и отдаёт его биндеру. Что взять из
// контекста — дело инструкции (и она вправе не брать ничего).
using PushFunc = std::function<void(const struct PushConstantBinder&, const struct PushInput&)>;

// Инструкции программы исполняются СТРОГО ПО ПОРЯДКУ, и порядок задаёт нумерацию слотов: биндер
// инкрементирует счётчик стадии на каждый Push*, а движковые блоки (UVL/params/раскладка)
// встают после них, с binder.frag_count. Значит порядок в списке = порядок register(bN)
// в шейдере, и перестановка инструкций молча переназначит регистры.
// Инструкция В СПИСКЕ ПРОГРАММЫ: слот уже назначен (позиция среди инструкций своей стадии),
// поэтому исполнение не зависит ни от порядка вызова, ни от того, сколько блоков пушат соседи.
struct PushInstruction {
    PushStage stage = PushStage::Fragment;
    Uint32    uniform_slot = 0;
    PushFunc  fn;
};
using PushInstructions = std::vector<PushInstruction>;

// Запись ПЛОСКОГО реестра ShaderManager — одна инструкция + программа, которой она принадлежит
// (по образцу BufferManager::UpdateInstruction, где инструкция держит свой BufferData*).
// Принадлежность — ИМЕНЕМ, а не указателем: программа пересоздаётся поимённо (LoadScene и
// редактор делают delete+create), поэтому указатель повис бы на первой же перезагрузке сцены,
// а имя переживает её. Резолв имени в программу — у потребителя, на сборке батчей.
struct ShaderPushInstruction {
    std::string program_name;
    PushStage   stage = PushStage::Fragment;
    PushFunc    fn;
};

struct DispatchSizeBinder {
    glm::uvec3 element_count{ 0, 0, 0 };
    // Кадровый слот — тот же контракт, что у PushConstantBinder::frame (см. выше).
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

// ── Потолки раскладки текстурных слотов (переключаемые варианты) ──
// Объявлены ЗДЕСЬ, один раз на весь движок: за ними приходят и BatchBuilder (нумерация ячеек,
// гарды переполнения), и модуль заливки состояний (страйд секции), и место создания шейдеров.
// В HLSL те же числа уезжают через ShaderDefine (см. выше) — дублировать литералом нельзя:
// разъезд C++ и байткода тихо перемешает секции состояний, без краша и без строки в логе.

// Сколько текстурных слотов может объявить один шейдер (размер slot_layout в пуше: 3 x uint4).
inline constexpr uint32_t MAX_SLOTS = 12;
// Сколько слотов ОДНОГО материала могут нести варианты — он же фиксированный страйд секции
// состояний. Фиксированный, а не по факту: смещение секции считается умножением на номер
// материала, поэтому переменная длина потребовала бы префиксной суммы в ключе батча.
inline constexpr uint32_t MAX_VARIATIVE_SLOTS = 4;
// Сколько UVL-блоков влезает в таблицу материала (размер textures[] в шейдере — константа
// компиляции, то есть потолок на ВСЕ материалы сразу).
inline constexpr uint32_t MAX_UVL_BLOCKS = 32;

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
