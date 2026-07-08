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
	VertexShaderData CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> bindings);
	FragmentShaderData CreateFragmentShader(const char* path);

	ShaderProgram* CreateShaderProgram(
		const std::string& name, const ShaderProgramDescription& spd, RenderPassStep* associated_pass,
		VertexShaderData vs, std::vector<BufferData*> vertex_shader_buffers,
		FragmentShaderData fs, std::vector<BufferData*> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots);

	ComputeShaderData CreateComputeShader(const char* path);
	// ������� �������� ComputeShaderProgram �� ���������� ������� �� ���������� � �������!
	// The order in which ComputeShaderPrograms are created does not determine the order in which they are executed in a pass!
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name, ComputeShaderData csd, 
		std::vector<BufferData*> rw_storage_buffers, 
		std::vector<BufferData*> ro_storage_buffers, 
		std::vector<ComputeShaderProgram::ComputeRWTextureBinding> rw_storage_textures, 
		std::vector<TextureAtlas*> ro_storage_textures, 
		std::vector<TextureAtlas*> texture_samplers, 
		ComputePassStep* associated_compute_pass);

	VertexShaderData CreateVertexShaderFromSPV(const char* path, std::initializer_list<VertexBufferBinding> bindings);
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
	VertexShaderData BuildVertexShader(const Uint8* spv, size_t spv_size, const char* dbg_name, std::initializer_list<VertexBufferBinding> bindings);

	FragmentShaderData BuildFragmentShader(const Uint8* spv, size_t spv_size, const char* dbg_name);

	ComputeShaderData BuildComputeShader(Uint8* spv, size_t spv_size, const char* dbg_name);

	std::string BuildCachePath(const char* source_path, uint64_t hash) const;
	void ReadVertexAttributes(std::initializer_list<ShaderBase::VertexBufferBinding> bindings, VertexShaderData& vs);

	Uint8* LoadOrCompileSPIRV(const char* hlsl_path, SDL_ShaderCross_ShaderStage stage, size_t& out_size);

	// Дедуп GPU-шейдеров по хэшу SPIR-V: одинаковый байткод → один SDL_GPUShader на всех
	// владельцев (ShaderData держит shared_ptr; здесь — неимущий weak-индекс для поиска).
	std::shared_ptr<SDL_GPUShader> LookupGpuShader(uint64_t key) const;
	std::shared_ptr<SDL_GPUShader> RegisterGpuShader(uint64_t key, SDL_GPUShader* raw);

	std::string m_cacheBasePath;

	std::unordered_map<std::string, std::unique_ptr<ShaderProgram>> shader_programs;
	
	std::vector<std::unique_ptr<ComputeShaderProgram>> compute_shader_programs;
	std::unordered_map<std::string, ComputeShaderProgram*>  compute_shader_programs_by_name;

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

