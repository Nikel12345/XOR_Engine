#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <functional>
#include <memory>
#include <SDL3/SDL_gpu.h>
#include "ShaderTypes.h"
#include "Aliases.h"

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
    // Потребляемые вершинные СТРИМЫ пула — КАНОНИЧНЫЕ имена буферов (BufferDataName = ключ
    // реестра BufferManager, указатель на строку, которой владеет пул). ПОРЯДОК = порядок
    // слотов пайплайна; бинд-шаг рендера биндит ровно этот список с нулевого слота
    // (BufferManager::BindGPUVertexBuffers). Заполняет CreateVertexShader из pull через пул.
    std::vector<BufferDataName> vertex_buffer_names;
    // Пул, из которого взяты стримы. Имя — для сериализации и формы редактора (ссылка по имени,
    // как у моделей). index_buffer отрезолвлен ЗДЕСЬ, на создании (у пула он один): сборка батча
    // читает готовое поле и потому не зависит от ModelManager, где живёт реестр пулов.
    std::string    pool_name;
    BufferDataName index_buffer = nullptr;
    // Дефайны компиляции — часть того же рецепта, что source_path/bindings/pool_name:
    // без них перекомпиляция из редактора или загрузка сцены собрала бы ДРУГОЙ шейдер
    // (сработали бы дефолты #ifndef), молча и без ошибки. Порядок канонизирован сортировкой
    // по имени в Create*Shader — от него зависит ключ кэша .spv.
    std::vector<ShaderDefine> defines;
    // ТИПОВЫЕ ПУШИ, объявленные самим исходником: маркеры //@push <тип> в порядке, в котором они
    // встречаются в развёрнутом тексте (файл + все его #include). Порядок значим — он и есть
    // порядок cbuffer'ов, то есть будущая нумерация слотов. Сейчас только читается и логируется.
    std::vector<std::string> push_kinds;
    bool dont_save = false;   // движковый дефолт (_fallback_vs) — в shaders.json не пишется
};

struct FragmentShaderData {
    ShaderBase::ShaderData shader_data;
    std::string source_path;   // исходник fs (для перекомпиляции из редактора; см. VertexShaderData)
    std::vector<ShaderDefine> defines;   // см. VertexShaderData::defines
    // ТИПОВЫЕ ПУШИ, объявленные самим исходником: маркеры //@push <тип> в порядке, в котором они
    // встречаются в развёрнутом тексте (файл + все его #include). Порядок значим — он и есть
    // порядок cbuffer'ов, то есть будущая нумерация слотов. Сейчас только читается и логируется.
    std::vector<std::string> push_kinds;
    bool dont_save = false;    // см. VertexShaderData::dont_save
};

struct ComputeShaderData {
	Uint8* spv_code = nullptr;
	size_t spv_size = 0;
    std::string source_path;   // исходник cs (для перекомпиляции/сериализации; см. VertexShaderData)
    std::vector<ShaderDefine> defines;   // см. VertexShaderData::defines
    Uint32 threadcount_x = 1;
    Uint32 threadcount_y = 1;
    Uint32 threadcount_z = 1;
    Uint32 num_samplers = 0;
    Uint32 num_readonly_storage_textures = 0;
    Uint32 num_readonly_storage_buffers = 0;
    Uint32 num_readwrite_storage_textures = 0;
    Uint32 num_readwrite_storage_buffers = 0;
    Uint32 num_uniform_buffers = 0;
    // ТИПОВЫЕ ПУШИ, объявленные самим исходником: маркеры //@push <тип> в порядке, в котором они
    // встречаются в развёрнутом тексте (файл + все его #include). Порядок значим — он и есть
    // порядок cbuffer'ов, то есть будущая нумерация слотов. Сейчас только читается и логируется.
    std::vector<std::string> push_kinds;
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

    // Push-инструкций у программы НЕТ полем: они лежат в плоском реестре ShaderManager под её
    // ИМЕНЕМ (ShaderPushInstruction), а сборка батчей собирает их оттуда. Копия здесь была бы
    // вторым источником истины про то же самое — и её пришлось бы синхронизировать на каждом
    // пересоздании программы.
	ShaderProgramDescription spd;   // ПО ЗНАЧЕНИЮ: параметры пайплайна живут в самом sp (не в словаре)
    // Проход — ССЫЛКА ПО ИМЕНИ, как vs_name/fs_name и буферы. Резолв в RenderPassStep* делают
    // потребители (PipeManager на сборке пайплайна, BatchBuilder на сборке батча), получая
    // PassManager параметром. Указатель здесь держать нельзя: имя обязано пережить сериализацию,
    // а восстанавливать его из RenderPassStep::debug_name запрещено — тот только для UI.
    RenderPassName render_pass_name;

    // ИНФОРМАЦИОННОЕ поле: только для логов, чтобы код, которому досталась голая программа
    // (PipeManager получает указатель без доступа к реестру), мог назвать её в сообщении.
    // Ключом реестра НЕ является и в логике не участвует — см. правило про debug_name в CLAUDE.md.
    std::string debug_name;
    bool dont_save = false;   // движковый дефолт (Lit/Skybox/_Fallback/…) — в shaders.json не пишется

};


struct ComputeShaderProgram {
    // ВСЕ ресурсы — ССЫЛКИ ПО ИМЕНИ, как vs_name/fs_name у ShaderProgram. Резолв в указатели
    // идёт на сборке батча (BatchBuilder::BuildComputeBatches), а не при создании: указатель
    // нельзя записать в манифест, а csp обязана сериализоваться так же, как render-sp.
    // Форма создания «по именам» (топ-левел тип в ShaderTypes.h) совпала с формой хранения —
    // отдельного ComputeRWTextureBinding больше нет, оба прежних написания остались алиасами.
    using ComputeRWTextureBindingParametr = ::ComputeRWTextureBindingParametr;
    using ComputeRWTextureBinding         = ::ComputeRWTextureBindingParametr;

    std::string cs_name;   // ссылка по имени на ComputeShaderData в реестре ShaderManager (см. ShaderProgram)
    std::vector<BufferDataName> rw_storage_buffer_names;
    std::vector<BufferDataName> ro_storage_buffer_names;
    std::vector<ComputeRWTextureBindingParametr> rw_storage_textures;
    std::vector<AtlasName> ro_storage_texture_names;
    std::vector<AtlasName> texture_sampler_names;

    // Ни push-, ни dispatch-инструкций программа полем не держит: они в плоском реестре
    // ShaderManager под её ИМЕНЕМ, сборка compute-батчей резолвит их оттуда (см. ShaderProgram).

    ComputePassName compute_pass_name;   // ссылка по имени, см. ShaderProgram::render_pass_name

    bool dont_save = false;   // см. ShaderProgram::dont_save

    // ИНФОРМАЦИОННОЕ поле: только для логов, чтобы код, которому досталась голая программа
    // (PipeManager получает указатель без доступа к реестру), мог назвать её в сообщении.
    // Ключом реестра НЕ является и в логике не участвует — см. правило про debug_name в CLAUDE.md.
    std::string debug_name;
};

// Ячейка реестра compute-программ. Имя лежит ЗДЕСЬ, а не в самой программе: у ShaderProgram его
// тоже нет — там реестр это словарь, и обход сам отдаёт ключ. У compute-программ реестр обязан
// быть УПОРЯДОЧЕННЫМ (порядок = порядок исполнения внутри прохода), поэтому вместо словаря вектор
// ячеек: обход так же отдаёт пару «имя + программа», и объекту незачем помнить собственный ключ.
struct ComputeProgramSlot {
    std::string name;
    std::unique_ptr<ComputeShaderProgram> program;
};
