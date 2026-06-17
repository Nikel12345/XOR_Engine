#pragma once
#include <string>
#include <initializer_list>
#include "ShaderData.h"
#include "Aliases.h"

class BufferManager;
class ShaderManager;
class PassManager;
class TextureManager;

// Узкий фасад для авторинга GPU-задач (шейдеры, рендер- и compute-программы).
// Backing-набор: Buffer / Shader / Pass / Texture — БЕЗ PipeManager и BatchBuilder.
// EngineContext держит его внутри и форвардит сюда; этим же фасадом пользуются
// ShaderSet'ы и (в перспективе) модуль физики, не таща весь EngineContext.
class GpuTaskContext {
public:
	GpuTaskContext(BufferManager* bm, ShaderManager* sm, PassManager* pm, TextureManager* tm);

	FragmentShaderData CreateFragmentShader(const char* hlsl_path);
	VertexShaderData CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> vertex_buffer_layout);
	ShaderProgramDescription* CreateShaderProgramDescription(const std::string& name);
	ShaderProgram* CreateShaderProgram(const std::string& name, ShaderProgramDescription* spd, const RenderPassName& associated_pass_name,
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

private:
	BufferManager* buffer_manager = nullptr;
	ShaderManager* shader_manager = nullptr;
	PassManager* pass_manager = nullptr;
	TextureManager* texture_manager = nullptr;
};
