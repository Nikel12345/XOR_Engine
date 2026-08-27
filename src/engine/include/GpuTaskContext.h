#pragma once
#include <string>
#include <initializer_list>
#include "ShaderData.h"
#include "Aliases.h"
#include "BufferData.h"
#include "BufferUpdateStruct.h"

class ShaderManager;
class PassManager;
class TextureManager;
class BufferManager;
class GeometryPool;


class GpuTaskContext {
public:
	GpuTaskContext(BufferManager* bm, ShaderManager* sm, PassManager* pm, TextureManager* tm);

	void CreateFragmentShader(const std::string& name, const char* hlsl_path, const ShaderDefines& defines = {});
	// Вершинник называет ПУЛ и потребляемые СЕМАНТИКИ; порядок слотов задаёт таблица стримов пула.
	// Пул приходит уже отрезолвленным: его реестр живёт в ModelManager, которого тут нет.
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
		const std::vector<ShaderBase::VertexSemantic>& pull, const ShaderDefines& defines = {});
	ShaderProgram* CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
		const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
		const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots);

	void CreateComputeShader(const std::string& name, const char* hlsl_path, const ShaderDefines& defines = {});
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name,
		const std::string& cs_name,
		std::initializer_list<BufferDataName> rw_storage_buffers,
		std::initializer_list<BufferDataName> ro_storage_buffers,
		std::initializer_list<ComputeShaderProgram::ComputeRWTextureBindingParametr> rw_storage_textures,
		std::initializer_list<AtlasName> ro_storage_textures,
		std::initializer_list<AtlasName> texture_samplers,
		const ComputePassName& associated_compute_pass, bool dont_save = false);

	// --- Буферы: создание и инструкции жизненного цикла (форвард в BufferManager) ---
	// Без usage-флагов: BufferData::usage наполняют декларации до бейка (см. BufferData.h).
	BufferData* CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour);
	BufferData* GetBufferData(BufferDataName name);

	void CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn = nullptr);
	void CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);
	void CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn);
	void CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn);

private:
	BufferManager* buffer_manager = nullptr;
	ShaderManager* shader_manager = nullptr;
	PassManager* pass_manager = nullptr;
	TextureManager* texture_manager = nullptr;
};
