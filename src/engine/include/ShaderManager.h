#pragma once
#include "ShaderData.h"
#include <unordered_map>
#include <cstdint>
#include <string>
#include "SDL3/SDL_gpu.h"
#include <memory>


class BufferManager;
class TextureManager;
class GeometryPool;
struct RenderPassStep;

class ShaderManager
{
public:
	ShaderManager(SDL_GPUDevice* device);
	// Create*Shader компилируют и КЛАДУТ результат в именованный реестр (vertex_shaders/
	// fragment_shaders/compute_shaders); sp/csp ссылаются на них по ИМЕНИ. Повтор с тем же именем
	// перезаписывает запись.
	// Хвостовой defines — дефайны препроцессора HLSL, подкидываемые компилятору на сборке
	// (ShaderDefine в ShaderTypes.h). Они же входят в ключ кэша .spv, см. LoadOrCompileSPIRV.
	//
	// Вершинник объявляет ПУЛ явно, а потребляемые стримы — СЕМАНТИКАМИ (pull): тем же языком
	// говорят манифест сцены и форма редактора, так что путь один на всех. Обратный вывод пула из
	// набора стримов больше не годится — POSITION есть в каждом пуле.
	// Порядок слотов задаёт ТАБЛИЦА СТРИМОВ пула (порядок pull не значим): слот со сдвинутым
	// буфером = чтение чужого страйда = UB, и решать это должен пул, а не порядок аргументов.
	// bm — на вызове (не полем): каждому выбранному стриму декларируется VERTEX, индексному
	// буферу пула — INDEX (см. BufferData::usage).
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
	                        const std::vector<ShaderBase::VertexSemantic>& pull, BufferManager* bm, ShaderDefines defines = {});
	void CreateFragmentShader(const std::string& name, const char* path, ShaderDefines defines = {});

	// bm передаётся НА ВЫЗОВЕ (не хранится полем): sp ссылается на буферы по имени, а usage-флаг
	// (GRAPHICS_STORAGE_READ) надо записать в саму обёртку BufferData. Владельцем связки менеджеров
	// остаётся EngineContext — менеджер не держит указателей на другие менеджеры.
	ShaderProgram* CreateShaderProgram(
		const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& render_pass_name,
		const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
		const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
		const std::vector<TextureSlotRole>& texture_slots, BufferManager* bm);

	void CreateComputeShader(const std::string& name, const char* path, ShaderDefines defines = {});
	// ПОРЯДОК СОЗДАНИЯ ЗНАЧИМ: BuildComputeBatches обходит compute_shader_programs по порядку и
	// складывает батчи в проход push_back'ом, а RenderManager исполняет их прямым обходом, без
	// сортировки. Т.е. csp, созданная раньше, и исполняется раньше внутри своего прохода — на этом
	// держится culling (csp_culling_clear обязан быть batch[0], иначе scatter видит ненулевые
	// счётчики → дубли строк на больших сценах). Сериализация обязана сохранять порядок массива.
	//
	// Ресурсы — ПО ИМЕНИ (см. ComputeShaderProgram). bm/tm нужны только чтобы записать usage-флаги
	// в сами обёртки буферов/атласов и отчитаться о промахах; менеджер их НЕ хранит (см. CLAUDE.md).
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
		std::vector<BufferDataName> rw_storage_buffers,
		std::vector<BufferDataName> ro_storage_buffers,
		std::vector<ComputeRWTextureBindingParametr> rw_storage_textures,
		std::vector<AtlasName> ro_storage_textures,
		std::vector<AtlasName> texture_samplers,
		const ComputePassName& compute_pass_name,
		BufferManager* bm, TextureManager* tm, bool dont_save = false);

	// Снос csp, за которые отвечает манифест сцены (dont_save == false) — загрузка их пересоздаёт
	// целиком, а не мержит: порядок внутри прохода значим (см. выше), и upsert по имени переставил
	// бы пересозданную программу в конец вектора. Кодовые/движковые (dont_save) переживают загрузку,
	// как и прочие ресурсы, которых нет в манифесте. Пайплайны инвалидирует ВЫЗЫВАЮЩИЙ до этого:
	// кэш PipeManager ключуется по csp*, а здесь объекты разрушаются.
	void ClearSavableComputeShaderPrograms();

	// Именованные шейдер-данные: доступ/удаление по имени (владелец — реестр, не sp/csp). Резолв
	// делают потребители на сборке (PipeManager/BatchBuilder). nullptr при промахе.
	VertexShaderData*   GetVertexShader(const std::string& name);
	FragmentShaderData* GetFragmentShader(const std::string& name);
	ComputeShaderData*  GetComputeShader(const std::string& name);

	// Занятость SD программами (по ссылкам-именам). Удаление ИСПОЛЬЗУЕМОГО шейдера запрещено:
	// пайплайн sp собран из его данных, а fallback с чужой раскладкой вершин невозможен —
	// сначала сними шейдер со всех sp (или удали их), потом удаляй SD.
	bool IsVertexShaderUsed(const std::string& name) const {
		for (auto& [n, sp] : shader_programs) if (sp->vs_name == name) return true;
		return false;
	}
	bool IsFragmentShaderUsed(const std::string& name) const {
		for (auto& [n, sp] : shader_programs) if (sp->fs_name == name) return true;
		return false;
	}
	bool IsComputeShaderUsed(const std::string& name) const {
		for (auto& slot : compute_shader_programs) if (slot.program && slot.program->cs_name == name) return true;
		return false;
	}

	// false = отказ (используется) или нет такой записи. Неиспользуемый SD ничего не рисует —
	// после удаления ни пайплайны, ни батчи трогать не нужно.
	bool DeleteVertexShader(const std::string& name) {
		if (IsVertexShaderUsed(name)) { SDL_Log("ShaderManager: vertex shader '%s' is used by a shader program — delete refused", name.c_str()); return false; }
		return vertex_shaders.erase(name) > 0;
	}
	bool DeleteFragmentShader(const std::string& name) {
		if (IsFragmentShaderUsed(name)) { SDL_Log("ShaderManager: fragment shader '%s' is used by a shader program — delete refused", name.c_str()); return false; }
		return fragment_shaders.erase(name) > 0;
	}
	bool DeleteComputeShader(const std::string& name);
	std::unordered_map<std::string, VertexShaderData>&   GetVertexShaders()   { return vertex_shaders; }
	std::unordered_map<std::string, FragmentShaderData>& GetFragmentShaders() { return fragment_shaders; }
	std::unordered_map<std::string, ComputeShaderData>&  GetComputeShaders()  { return compute_shaders; }

	VertexShaderData CreateVertexShaderFromSPV(const char* path, std::initializer_list<ShaderBase::VertexBufferBinding> bindings);
	FragmentShaderData CreateFragmentShaderFromSPV(const char* spv_path);
	ComputeShaderData CreateComputeShaderFromSPV(const char* spv_path);

	ShaderProgram* GetShaderProgram(const ShaderName& name);
	// Удаление sp: erase из словаря разрушает ShaderProgram → отпускает его ShaderData
	// (shared_ptr<SDL_GPUShader>). Шейдеры релизятся ПО REFCOUNT: неиспользуемые — освобождаются,
	// переиспользуемые (общий vs) — живут. Пайплайн sp освобождает вызывающий (PipeManager,
	// отложенно) ДО этого erase (кэш пайплайнов ключуется по sp*).
	void DeleteShaderProgram(const std::string& name) { shader_programs.erase(name); }

	ComputeShaderProgram* GetComputeShaderProgram(const std::string& name);

	// ── Код-байндинги (push/dispatch) — ИНСТРУКЦИИ ПО ИМЕНИ, как resize-инструкции атласов ──
	// Лямбды живут в игровом коде и НЕ сериализуются: sp/csp из манифеста сцены рождается
	// голым. Поэтому функция регистрируется ОДИН РАЗ (Game::Init), а привязка идёт сама — на создании
	// программы И в BindShaderFunctions в конце LoadScene. Порядок «функция раньше программы» или
	// наоборот значения не имеет; программы нет вовсе — запись ждёт её и попадает в отчёт,
	// а не теряется молча, как при разовом колбэке с if (sp) внутри.
	//
	// Ключ = ИМЯ программы, тот же, что в реестрах shader_programs/compute_shader_programs: пространство
	// имён программ уже глобально (LoadScene делает merge-upsert в тот же словарь), так что
	// реестр функций наследует ровно тот же контракт уникальности, а не вводит новый. Захотим
	// сцено-локальные имена — менять надо ключ САМИХ программ, и эти словари пойдут за ним.
	using PushFunc     = std::function<void(const PushConstantBinder&, const void*)>;
	using DispatchFunc = std::function<void(DispatchSizeBinder&, const void*)>;

	// Сырая форма: лямбда сама разбирается с raw (или игнорирует его — тело MAIN_PASS передаёт nullptr).
	void CreatePushFunc(const std::string& sp_name, PushFunc fn);
	// Типизированная: T — структура push-данных прохода, стирание типа делает обёртка
	// (как ShaderProgram::BindPushConstants<T>, который остался для прямой правки готовой sp).
	template<typename T, typename Fn> void CreatePushFunc(const std::string& sp_name, Fn&& fn) {
		CreatePushFunc(sp_name, PushFunc([fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const void* raw) {
			fn(b, *static_cast<const T*>(raw));
		}));
	}

	void CreateComputePushFunc(const std::string& csp_name, PushFunc fn);
	template<typename T, typename Fn> void CreateComputePushFunc(const std::string& csp_name, Fn&& fn) {
		CreateComputePushFunc(csp_name, PushFunc([fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const void* raw) {
			fn(b, *static_cast<const T*>(raw));
		}));
	}

	void CreateDispatchFunc(const std::string& csp_name, DispatchFunc fn);
	template<typename T, typename Fn> void CreateDispatchFunc(const std::string& csp_name, Fn&& fn) {
		CreateDispatchFunc(csp_name, DispatchFunc([fn = std::forward<Fn>(fn)](DispatchSizeBinder& b, const void* raw) {
			fn(b, *static_cast<const T*>(raw));
		}));
	}

	// Пере-привязка всех зарегистрированных функций к текущим программам + лог записей, которым
	// программы не нашлось (тот самый случай «функция есть, шейдера нет»). Зовёт Engine в конце LoadScene.
	void BindShaderFunctions();

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& GetShaderPrograms() { return shader_programs; }
	// Обход отдаёт пару «имя + программа» (см. ComputeProgramSlot) — как у GetShaderPrograms().
	std::vector<ComputeProgramSlot>& GetComputeShaderPrograms() { return compute_shader_programs; };

	bool IsDirtyGraphicsPipelines() const { return dirty_graphics_pipelines; }
	void SetDirtyGraphicsPipelines(bool dirty) { dirty_graphics_pipelines = dirty; }
	bool IsDirtyComputePipelines() const { return dirty_compute_pipelines; }
	void SetDirtyComputePipelines(bool dirty) { dirty_compute_pipelines = dirty; }
	// Отдельный флаг для пересборки compute-БАТЧЕЙ (а не пайплайнов). Пайплайны строятся один раз
	// (зависят только от шейдера), а батчи снапшотят SDL_GPUTexture* атласов и должны пересобираться
	// при создании программ И при ресайзе (текстуры пересоздаются). Разделён с pipeline-флагом, иначе
	// CreateComputePipelines глотал бы dirty раньше BuildComputeBatches.
	bool IsDirtyComputeBatches() const { return dirty_compute_batches; }
	void SetDirtyComputeBatches(bool dirty) { dirty_compute_batches = dirty; }

	~ShaderManager();

private:
	VertexShaderData BuildVertexShader(const Uint8* spv, size_t spv_size, const char* dbg_name, const std::vector<ShaderBase::VertexBufferBinding>& bindings);

	FragmentShaderData BuildFragmentShader(const Uint8* spv, size_t spv_size, const char* dbg_name);

	ComputeShaderData BuildComputeShader(Uint8* spv, size_t spv_size, const char* dbg_name);

	std::string BuildCachePath(const char* source_path, uint64_t hash) const;
	void ReadVertexAttributes(const std::vector<ShaderBase::VertexBufferBinding>& bindings, VertexShaderData& vs);

	Uint8* LoadOrCompileSPIRV(const char* hlsl_path, SDL_ShaderCross_ShaderStage stage, size_t& out_size,
	                          ShaderDefines defines);

	// Дедуп GPU-шейдеров по хэшу SPIR-V: одинаковый байткод → один SDL_GPUShader на всех
	// владельцев (ShaderData держит shared_ptr; здесь — неимущий weak-индекс для поиска).
	std::shared_ptr<SDL_GPUShader> LookupGpuShader(uint64_t key) const;
	std::shared_ptr<SDL_GPUShader> RegisterGpuShader(uint64_t key, SDL_GPUShader* raw);

	std::string m_cacheBasePath;

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>> shader_programs;

	// Единственный реестр compute-программ: упорядоченный (порядок исполнения) и он же
	// именованный. Отдельного индекса по имени НЕТ намеренно — второй источник истины про то же
	// самое рассинхронизируется, а программ десятки, линейный поиск на холодных путях бесплатен.
	std::vector<ComputeProgramSlot> compute_shader_programs;

	// Реестры именованных шейдер-данных — владельцы. compute_shaders владеет сырым spv_code
	// (free в деструкторе идёт отсюда, а не с csp, т.к. csp держит только имя).
	std::unordered_map<std::string, VertexShaderData>   vertex_shaders;
	std::unordered_map<std::string, FragmentShaderData> fragment_shaders;
	std::unordered_map<std::string, ComputeShaderData>  compute_shaders;

	SDL_GPUDevice* dev;

	// Инструкции код-байндингов по имени программы (см. CreatePushFunc): переживают пересоздание
	// самих программ — 1 программа = 1 функция (повторная регистрация перезаписывает).
	std::unordered_map<std::string, PushFunc>     push_instructions_;
	std::unordered_map<std::string, PushFunc>     compute_push_instructions_;
	std::unordered_map<std::string, DispatchFunc> dispatch_instructions_;

	std::unordered_map<uint64_t, std::weak_ptr<SDL_GPUShader>> gpu_shaders;
	// Токен живости: делитер шейдера освобождает GPU-ресурс лишь пока менеджер жив (а с ним и
	// device). Поздние релизы (статик-копия main_pass_vs при выходе из программы) → no-op;
	// такие ресурсы добьёт уничтожение device. Гасится в ~ShaderManager ПОСЛЕ shader_programs.
	std::shared_ptr<int> shader_alive_ = std::make_shared<int>(0);

	bool dirty_graphics_pipelines = true;
	bool dirty_compute_pipelines = true;
	bool dirty_compute_batches = true;
};

