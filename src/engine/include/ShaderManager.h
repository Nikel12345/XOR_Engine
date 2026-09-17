#pragma once
#include "ShaderData.h"
#include "SDL3_shadercross/SDL_shadercross.h"
#include "ResourceRegistry.h"
#include <unordered_map>
#include <cstdint>
#include <string>
#include "SDL3/SDL_gpu.h"
#include <memory>


class BufferManager;
class TextureManager;
class GeometryPool;
struct RenderPassStep;

struct VertexShaderCell   { std::string name; std::unique_ptr<VertexShaderData>   object; };
struct FragmentShaderCell { std::string name; std::unique_ptr<FragmentShaderData> object; };
struct ComputeShaderCell  { std::string name; std::unique_ptr<ComputeShaderData>  object; };

using VertexShaderRegistry   = ResourceRegistry<VertexShaderCell,   VertexShaderId>;
using FragmentShaderRegistry = ResourceRegistry<FragmentShaderCell, FragmentShaderId>;
using ComputeShaderRegistry  = ResourceRegistry<ComputeShaderCell,  ComputeShaderId>;
using ShaderProgramRegistry  = ResourceRegistry<ShaderProgramCell,  ShaderProgramId>;
using ComputeProgramRegistry = ResourceRegistry<ComputeProgramCell, ComputeProgramId>;

class ShaderManager
{
public:
	ShaderManager(SDL_GPUDevice* device);
	// Повтор с тем же именем перезаписывает запись реестра.
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
	                        const std::vector<ShaderBase::VertexSemantic>& pull, BufferManager* bm, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);
	void CreateFragmentShader(const std::string& name, const char* path, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);

	ShaderProgram* CreateShaderProgram(
		const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& render_pass_name,
		const std::string& vs_name, std::vector<BufferDataName> vertex_shader_buffer_names,
		const std::string& fs_name, std::vector<BufferDataName> fragment_shader_buffer_names,
		const std::vector<TextureSlotRole>& texture_slots, BufferManager* bm, ResourceTag tags = ResourceTag::None);

	void CreateComputeShader(const std::string& name, const char* path, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);

	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
		std::vector<BufferDataName> rw_storage_buffers,
		std::vector<BufferDataName> ro_storage_buffers,
		std::vector<ComputeRWTextureBindingParametr> rw_storage_textures,
		std::vector<AtlasName> ro_storage_textures,
		std::vector<AtlasName> texture_samplers,
		const ComputePassName& compute_pass_name,
		BufferManager* bm, TextureManager* tm, ResourceTag tags = ResourceTag::None);

	size_t ClearSceneShaders();

	VertexShaderData*   GetVertexShader(VertexShaderId id) const   { return vertex_shaders.Get(id); }
	FragmentShaderData* GetFragmentShader(FragmentShaderId id) const { return fragment_shaders.Get(id); }
	ComputeShaderData*  GetComputeShader(ComputeShaderId id) const   { return compute_shaders.Get(id); }
	VertexShaderData*   GetVertexShader(const std::string& name);
	FragmentShaderData* GetFragmentShader(const std::string& name);
	ComputeShaderData*  GetComputeShader(const std::string& name);

	VertexShaderId   InternVertexShader(const std::string& name)   { return vertex_shaders.Intern(name); }
	FragmentShaderId InternFragmentShader(const std::string& name) { return fragment_shaders.Intern(name); }
	ComputeShaderId  InternComputeShader(const std::string& name)  { return compute_shaders.Intern(name); }

	const VertexShaderRegistry&   VertexShaders() const   { return vertex_shaders; }
	const FragmentShaderRegistry& FragmentShaders() const { return fragment_shaders; }
	const ComputeShaderRegistry&  ComputeShaders() const  { return compute_shaders; }

	bool IsVertexShaderUsed(VertexShaderId id) const {
		for (int32_t i = 0; i < shader_programs.Count(); ++i)
			if (const ShaderProgram* sp = shader_programs.At(i).object.get(); sp && sp->vs_id == id) return true;
		return false;
	}
	bool IsFragmentShaderUsed(FragmentShaderId id) const {
		for (int32_t i = 0; i < shader_programs.Count(); ++i)
			if (const ShaderProgram* sp = shader_programs.At(i).object.get(); sp && sp->fs_id == id) return true;
		return false;
	}
	bool IsComputeShaderUsed(ComputeShaderId id) const {
		for (int32_t i = 0; i < compute_shader_programs.Count(); ++i)
			if (const ComputeShaderProgram* p = compute_shader_programs.At(i).object.get(); p && p->cs_id == id) return true;
		return false;
	}

	bool DeleteVertexShader(VertexShaderId id, NameSlot slot) {
		if (IsVertexShaderUsed(id)) { SDL_Log("ShaderManager: vertex shader '%s' is used by a shader program — delete refused", vertex_shaders.NameOf(id).c_str()); return false; }
		return Release(vertex_shaders, id, slot);
	}
	bool DeleteFragmentShader(FragmentShaderId id, NameSlot slot) {
		if (IsFragmentShaderUsed(id)) { SDL_Log("ShaderManager: fragment shader '%s' is used by a shader program — delete refused", fragment_shaders.NameOf(id).c_str()); return false; }
		return Release(fragment_shaders, id, slot);
	}
	bool DeleteComputeShader(ComputeShaderId id, NameSlot slot);

	bool RenameVertexShader(VertexShaderId id, const std::string& new_name)   { return Rename(vertex_shaders, id, new_name); }
	bool RenameFragmentShader(FragmentShaderId id, const std::string& new_name) { return Rename(fragment_shaders, id, new_name); }
	bool RenameComputeShader(ComputeShaderId id, const std::string& new_name)  { return Rename(compute_shaders, id, new_name); }

	VertexShaderData CreateVertexShaderFromSPV(const char* path, std::initializer_list<ShaderBase::VertexBufferBinding> bindings);
	FragmentShaderData CreateFragmentShaderFromSPV(const char* spv_path);
	ComputeShaderData CreateComputeShaderFromSPV(const char* spv_path);

	ShaderProgram* GetShaderProgram(const ShaderName& name);
	ShaderProgram* GetShaderProgram(ShaderProgramId id) const { return shader_programs.Get(id); }
	ShaderProgramId    ShaderProgramIdOf(const std::string& name) const { return shader_programs.Find(name); }
	ShaderProgramId    InternShaderProgram(const std::string& name)     { return shader_programs.Intern(name); }
	const std::string& ShaderProgramNameOf(ShaderProgramId id) const    { return shader_programs.NameOf(id); }
	bool DeleteShaderProgram(ShaderProgramId id, NameSlot slot) { return Release(shader_programs, id, slot); }
	bool RenameShaderProgram(ShaderProgramId id, const std::string& new_name) { return Rename(shader_programs, id, new_name); }

	ComputeShaderProgram* GetComputeShaderProgram(const std::string& name);
	ComputeShaderProgram* GetComputeShaderProgram(ComputeProgramId id) const { return compute_shader_programs.Get(id); }
	ComputeProgramId   ComputeProgramIdOf(const std::string& name) const { return compute_shader_programs.Find(name); }
	const std::string& ComputeProgramNameOf(ComputeProgramId id) const   { return compute_shader_programs.NameOf(id); }

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

	ShaderProgramRegistry&  ShaderPrograms()  { return shader_programs; }
	ComputeProgramRegistry& ComputePrograms() { return compute_shader_programs; }

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

	Uint8* LoadOrCompileSPIRV(const char* hlsl_path, SDL_ShaderCross_ShaderStage stage, size_t& out_size,
	                          const ShaderDefines& defines,
	                          std::vector<std::string>* out_push_kinds = nullptr);

	std::shared_ptr<SDL_GPUShader> LookupGpuShader(uint64_t key) const;
	std::shared_ptr<SDL_GPUShader> RegisterGpuShader(uint64_t key, SDL_GPUShader* raw);

	std::string m_cacheBasePath;

	ShaderProgramRegistry  shader_programs;
	ComputeProgramRegistry compute_shader_programs;

	// compute_shaders владеет сырым spv_code: free в деструкторе идёт отсюда.
	VertexShaderRegistry   vertex_shaders;
	FragmentShaderRegistry fragment_shaders;
	ComputeShaderRegistry  compute_shaders;

	template <class Registry, class Id>
	static bool Rename(Registry& reg, Id id, const std::string& new_name) {
		if (!reg.Get(id) || new_name.empty()) return false;
		if (!reg.Rename(id, new_name)) { SDL_Log("ShaderManager: name '%s' is already taken", new_name.c_str()); return false; }
		return true;
	}

	template <class Registry, class Id>
	static bool Release(Registry& reg, Id id, NameSlot slot) {
		return slot == NameSlot::Release ? reg.Drop(id) : reg.Clear(id);
	}

	SDL_GPUDevice* dev;

	std::unordered_map<std::string, PushKind> push_kinds_;

	std::vector<ShaderPushInstruction> push_instructions_;
	std::vector<ShaderPushInstruction> compute_push_instructions_;
	std::unordered_map<std::string, DispatchFunc> dispatch_instructions_;

	std::unordered_map<uint64_t, std::weak_ptr<SDL_GPUShader>> gpu_shaders;

	bool dirty_graphics_pipelines = true;
	bool dirty_compute_pipelines = true;
	bool dirty_compute_batches = true;
};

