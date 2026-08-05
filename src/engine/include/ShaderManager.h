#pragma once
#include "ShaderData.h"
#include <unordered_map>
#include <cstdint>
#include <string>
#include "SDL3/SDL_gpu.h"
#include <memory>


class BufferManager;
struct RenderPassStep;

class ShaderManager
{
public:
	ShaderManager(SDL_GPUDevice* device);
	// Create*Shader компилируют и КЛАДУТ результат в именованный реестр (vertex_shaders/
	// fragment_shaders/compute_shaders); sp/csp ссылаются на них по ИМЕНИ. Повтор с тем же именем
	// перезаписывает запись.
	//
	// Вершинник объявляет ПОТРЕБЛЯЕМЫЕ СТРИМЫ пула перечислением имён вершинных буферов
	// (порядок списка = порядок слотов пайплайна; раскладка слота — из таблицы PosUVNormPool).
	// Тени перечисляют один Pos-стрим; main — все три. ПОРЯДОК КРИТИЧЕН: слот со сдвинутым
	// буфером = чтение чужого страйда = UB. bm — на вызове (не полем): каждому названному буферу
	// декларируется VERTEX, индексному буферу их пула — INDEX (см. BufferData::usage).
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const std::vector<std::string>& vertex_buffer_names, BufferManager* bm);
	void CreateFragmentShader(const std::string& name, const char* path);

	// bm передаётся НА ВЫЗОВЕ (не хранится полем): sp ссылается на буферы по имени, а usage-флаг
	// (GRAPHICS_STORAGE_READ) надо записать в саму обёртку BufferData. Владельцем связки менеджеров
	// остаётся EngineContext — менеджер не держит указателей на другие менеджеры.
	ShaderProgram* CreateShaderProgram(
		const std::string& name, const ShaderProgramDescription& spd, RenderPassStep* associated_pass,
		const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
		const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
		const std::vector<TextureSlotRole>& texture_slots, BufferManager* bm);

	void CreateComputeShader(const std::string& name, const char* path);
	// The order in which ComputeShaderPrograms are created does not determine the order in which they are executed in a pass!
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
		std::vector<BufferData*> rw_storage_buffers,
		std::vector<BufferData*> ro_storage_buffers,
		std::vector<ComputeShaderProgram::ComputeRWTextureBinding> rw_storage_textures,
		std::vector<TextureAtlas*> ro_storage_textures,
		std::vector<TextureAtlas*> texture_samplers,
		ComputePassStep* associated_compute_pass);

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
		for (auto& csp : compute_shader_programs) if (csp->cs_name == name) return true;
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

	ShaderProgram* GetShaderProgram(const std::string& name);
	// Удаление sp: erase из словаря разрушает ShaderProgram → отпускает его ShaderData
	// (shared_ptr<SDL_GPUShader>). Шейдеры релизятся ПО REFCOUNT: неиспользуемые — освобождаются,
	// переиспользуемые (общий vs) — живут. Пайплайн sp освобождает вызывающий (PipeManager,
	// отложенно) ДО этого erase (кэш пайплайнов ключуется по sp*).
	void DeleteShaderProgram(const std::string& name) { shader_programs.erase(name); }

	ComputeShaderProgram* GetComputeShaderProgram(const std::string& name);

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& GetShaderPrograms() { return shader_programs; }
	std::vector<std::unique_ptr<ComputeShaderProgram>>& GetComputeShaderPrograms() { return compute_shader_programs; };

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

	Uint8* LoadOrCompileSPIRV(const char* hlsl_path, SDL_ShaderCross_ShaderStage stage, size_t& out_size);

	// Дедуп GPU-шейдеров по хэшу SPIR-V: одинаковый байткод → один SDL_GPUShader на всех
	// владельцев (ShaderData держит shared_ptr; здесь — неимущий weak-индекс для поиска).
	std::shared_ptr<SDL_GPUShader> LookupGpuShader(uint64_t key) const;
	std::shared_ptr<SDL_GPUShader> RegisterGpuShader(uint64_t key, SDL_GPUShader* raw);

	std::string m_cacheBasePath;

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>> shader_programs;

	std::vector<std::unique_ptr<ComputeShaderProgram>> compute_shader_programs;
	std::unordered_map<std::string, ComputeShaderProgram*>  compute_shader_programs_by_name;

	// Реестры именованных шейдер-данных — владельцы. compute_shaders владеет сырым spv_code
	// (free в деструкторе идёт отсюда, а не с csp, т.к. csp держит только имя).
	std::unordered_map<std::string, VertexShaderData>   vertex_shaders;
	std::unordered_map<std::string, FragmentShaderData> fragment_shaders;
	std::unordered_map<std::string, ComputeShaderData>  compute_shaders;

	SDL_GPUDevice* dev;

	std::unordered_map<uint64_t, std::weak_ptr<SDL_GPUShader>> gpu_shaders;
	// Токен живости: делитер шейдера освобождает GPU-ресурс лишь пока менеджер жив (а с ним и
	// device). Поздние релизы (статик-копия main_pass_vs при выходе из программы) → no-op;
	// такие ресурсы добьёт уничтожение device. Гасится в ~ShaderManager ПОСЛЕ shader_programs.
	std::shared_ptr<int> shader_alive_ = std::make_shared<int>(0);

	bool dirty_graphics_pipelines = true;
	bool dirty_compute_pipelines = true;
	bool dirty_compute_batches = true;
};

