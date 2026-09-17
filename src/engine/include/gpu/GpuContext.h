#pragma once
#include <string>
#include <initializer_list>
#include "ShaderData.h"
#include "Aliases.h"
#include "BufferData.h"
#include "BufferUpdateStruct.h"

class ShaderManager;
class PassManager;
class PipeManager;
class TextureManager;
class BufferManager;
class GeometryPool;
struct TextureAtlas;

class GpuContext {
public:
	GpuContext(BufferManager* bm, ShaderManager* sm, PassManager* pass, PipeManager* pipe, TextureManager* tm);

	TextureAtlas* CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name, ResourceTag tags = ResourceTag::None);
	TextureAtlas* CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name, ResourceTag tags = ResourceTag::None);
	TextureAtlas* GetTextureAtlas(const AtlasName& name) const;

	void CreateGraphicsPipelines();
	void CreateComputePipelines();

	ShaderProgramId InternShaderProgram(const std::string& name);

	// Отдаются наружу потому, что их некому больше держать: выше живёт EngineContext, и своей
	// копии этих указателей у него нет. Операции над буферами и проходами — здесь же, не через них.
	BufferManager* GetBufferManager() const { return buffer_manager; }
	PassManager*   GetPassManager()   const { return pass_manager; }

	void CreateFragmentShader(const std::string& name, const char* hlsl_path, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
		const std::vector<ShaderBase::VertexSemantic>& pull, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);
	ShaderProgram* CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
		const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
		const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots, ResourceTag tags = ResourceTag::None);

	void CreateComputeShader(const std::string& name, const char* hlsl_path, const ShaderDefines& defines = {}, ResourceTag tags = ResourceTag::None);
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name,
		const std::string& cs_name,
		std::initializer_list<BufferDataName> rw_storage_buffers,
		std::initializer_list<BufferDataName> ro_storage_buffers,
		std::initializer_list<ComputeRWTextureBindingParametr> rw_storage_textures,
		std::initializer_list<AtlasName> ro_storage_textures,
		std::initializer_list<AtlasName> texture_samplers,
		const ComputePassName& associated_compute_pass, ResourceTag tags = ResourceTag::None);

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
	PipeManager* pipe_manager = nullptr;
	TextureManager* texture_manager = nullptr;
};
