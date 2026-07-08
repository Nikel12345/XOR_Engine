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


class GpuTaskContext {
public:
	GpuTaskContext(BufferManager* bm, ShaderManager* sm, PassManager* pm, TextureManager* tm);

	FragmentShaderData CreateFragmentShader(const char* hlsl_path);
	VertexShaderData CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> vertex_buffer_layout);
	ShaderProgram* CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
		VertexShaderData vs, std::initializer_list<BufferDataName> vertex_shader_buffers,
		FragmentShaderData fs, std::initializer_list<BufferDataName> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots);

	ComputeShaderData CreateComputeShader(const char* hlsl_path);
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name,
		ComputeShaderData cs,
		std::initializer_list<BufferDataName> rw_storage_buffers,
		std::initializer_list<BufferDataName> ro_storage_buffers,
		std::initializer_list<ComputeShaderProgram::ComputeRWTextureBindingParametr> rw_storage_textures,
		std::initializer_list<AtlasName> ro_storage_textures,
		std::initializer_list<AtlasName> texture_samplers,
		const ComputePassName& associated_compute_pass);

	// --- Буферы: создание и инструкции жизненного цикла (форвард в BufferManager) ---
	BufferData* CreateBufferData(BufferDataName name, Uint32 size, SDL_GPUBufferUsageFlags usage, BufferDataType type, ResizeBehaviour resize_behaviour);
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
