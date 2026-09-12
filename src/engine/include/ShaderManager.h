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
	// Повтор с тем же именем перезаписывает запись реестра.
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
	                        const std::vector<ShaderBase::VertexSemantic>& pull, BufferManager* bm, const ShaderDefines& defines = {});
	void CreateFragmentShader(const std::string& name, const char* path, const ShaderDefines& defines = {});

	ShaderProgram* CreateShaderProgram(
		const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& render_pass_name,
		const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
		const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
		const std::vector<TextureSlotRole>& texture_slots, BufferManager* bm);

	void CreateComputeShader(const std::string& name, const char* path, const ShaderDefines& defines = {});

	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
		std::vector<BufferDataName> rw_storage_buffers,
		std::vector<BufferDataName> ro_storage_buffers,
		std::vector<ComputeRWTextureBindingParametr> rw_storage_textures,
		std::vector<AtlasName> ro_storage_textures,
		std::vector<AtlasName> texture_samplers,
		const ComputePassName& compute_pass_name,
		BufferManager* bm, TextureManager* tm, bool dont_save = false);

	// Сцена пересоздаёт свои csp целиком: upsert по имени переставил бы программу в конец вектора,
	// то есть в конец очереди исполнения. Пайплайны вызывающий обязан инвалидировать ДО вызова —
	// здесь объекты разрушаются.
	void ClearSavableComputeShaderPrograms();

	VertexShaderData*   GetVertexShader(const std::string& name);
	FragmentShaderData* GetFragmentShader(const std::string& name);
	ComputeShaderData*  GetComputeShader(const std::string& name);

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

	// false = отказ (используется) или нет такой записи. После удаления ни пайплайны, ни батчи
	// трогать не нужно.
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
	void DeleteShaderProgram(const std::string& name) { shader_programs.erase(name); }

	ComputeShaderProgram* GetComputeShaderProgram(const std::string& name);

	using PushFunc     = ::PushFunc;
	using DispatchFunc = std::function<void(DispatchSizeBinder&, const void*)>;

	// Каждая регистрация ДОБАВЛЯЕТ запись: повторный вызов удлинит список и сдвинет регистры,
	// поэтому инструкции регистрируются один раз, на инициализации.
	void CreatePushInstruction(const std::string& sp_name, PushStage stage, PushFunc fn);
	template<typename T, typename Fn> void CreatePushInstruction(const std::string& sp_name, PushStage stage, Fn&& fn) {
		CreatePushInstruction(sp_name, stage, PushFunc([fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const PushInput& in) {
			fn(b, *static_cast<const T*>(in.pass_state));
		}));
	}

	void CreateComputePushInstruction(const std::string& csp_name, PushFunc fn);
	template<typename T, typename Fn> void CreateComputePushInstruction(const std::string& csp_name, Fn&& fn) {
		CreateComputePushInstruction(csp_name, PushFunc([fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const PushInput& in) {
			fn(b, *static_cast<const T*>(in.pass_state));
		}));
	}

	void CreateDispatchInstruction(const std::string& csp_name, DispatchFunc fn);
	template<typename T, typename Fn> void CreateDispatchInstruction(const std::string& csp_name, Fn&& fn) {
		CreateDispatchInstruction(csp_name, DispatchFunc([fn = std::forward<Fn>(fn)](DispatchSizeBinder& b, const void* raw) {
			fn(b, *static_cast<const T*>(raw));
		}));
	}


	struct PushKind {
		PushStage stage;
		PushFunc  fn;
	};
	void RegisterPushKind(const std::string& kind, PushStage stage, PushFunc fn);
	template<typename T, typename Fn> void RegisterPushKind(const std::string& kind, PushStage stage, Fn&& fn) {
		RegisterPushKind(kind, stage, PushFunc([fn = std::forward<Fn>(fn)](const PushConstantBinder& b, const PushInput& in) {
			fn(b, *static_cast<const T*>(in.pass_state));
		}));
	}

	PushInstructions CollectPushInstructions(const std::string& sp_name) const;
	PushInstructions CollectComputePushInstructions(const std::string& csp_name) const;
	DispatchFunc     GetDispatchInstruction(const std::string& csp_name) const;

	void ReportOrphanCodeBindings();

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>>& GetShaderPrograms() { return shader_programs; }
	std::vector<ComputeProgramSlot>& GetComputeShaderPrograms() { return compute_shader_programs; };

	bool IsDirtyGraphicsPipelines() const { return dirty_graphics_pipelines; }
	void SetDirtyGraphicsPipelines(bool dirty) { dirty_graphics_pipelines = dirty; }
	bool IsDirtyComputePipelines() const { return dirty_compute_pipelines; }
	void SetDirtyComputePipelines(bool dirty) { dirty_compute_pipelines = dirty; }
	bool IsDirtyComputeBatches() const { return dirty_compute_batches; }
	void SetDirtyComputeBatches(bool dirty) { dirty_compute_batches = dirty; }

	~ShaderManager();

private:
	VertexShaderData BuildVertexShader(const Uint8* spv, size_t spv_size, const char* dbg_name, const std::vector<ShaderBase::VertexBufferBinding>& bindings);

	FragmentShaderData BuildFragmentShader(const Uint8* spv, size_t spv_size, const char* dbg_name);

	ComputeShaderData BuildComputeShader(Uint8* spv, size_t spv_size, const char* dbg_name);

	std::string BuildCachePath(const char* source_path, uint64_t hash) const;
	void ReadVertexAttributes(const std::vector<ShaderBase::VertexBufferBinding>& bindings, VertexShaderData& vs);

	// slots_raw — счётчик слотов сборки; void*, чтобы служебный тип не торчал в заголовке.
	void AddKindInstructions(PushInstructions& out, void* slots_raw,
		const std::vector<std::string>& kinds, const std::string& owner) const;

	// out_push_kinds заполняется и при попадании в кэш .spv.
	Uint8* LoadOrCompileSPIRV(const char* hlsl_path, SDL_ShaderCross_ShaderStage stage, size_t& out_size,
	                          const ShaderDefines& defines,
	                          std::vector<std::string>* out_push_kinds = nullptr);

	std::shared_ptr<SDL_GPUShader> LookupGpuShader(uint64_t key) const;
	std::shared_ptr<SDL_GPUShader> RegisterGpuShader(uint64_t key, SDL_GPUShader* raw);

	std::string m_cacheBasePath;

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>> shader_programs;

	std::vector<ComputeProgramSlot> compute_shader_programs;

	// compute_shaders владеет сырым spv_code: free в деструкторе идёт отсюда.
	std::unordered_map<std::string, VertexShaderData>   vertex_shaders;
	std::unordered_map<std::string, FragmentShaderData> fragment_shaders;
	std::unordered_map<std::string, ComputeShaderData>  compute_shaders;

	SDL_GPUDevice* dev;

	std::unordered_map<std::string, PushKind> push_kinds_;

	// Записи переживают пересоздание программ: на них никто не ссылается, принадлежность — имя.
	std::vector<ShaderPushInstruction> push_instructions_;
	std::vector<ShaderPushInstruction> compute_push_instructions_;
	std::unordered_map<std::string, DispatchFunc> dispatch_instructions_;

	std::unordered_map<uint64_t, std::weak_ptr<SDL_GPUShader>> gpu_shaders;

	bool dirty_graphics_pipelines = true;
	bool dirty_compute_pipelines = true;
	bool dirty_compute_batches = true;
};

