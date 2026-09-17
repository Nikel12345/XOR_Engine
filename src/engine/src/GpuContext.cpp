#include "PCH.h"
#include "GpuContext.h"
#include "BufferManager.h"
#include "ShaderManager.h"
#include "PassManager.h"
#include "PipeManager.h"
#include "TextureManager.h"

using namespace ShaderBase;

GpuContext::GpuContext(BufferManager* bm, ShaderManager* sm, PassManager* pass, PipeManager* pipe, TextureManager* tm)
	: buffer_manager(bm), shader_manager(sm), pass_manager(pass), pipe_manager(pipe), texture_manager(tm)
{
}

TextureAtlas* GpuContext::GetTextureAtlas(const AtlasName& name) const
{
	return texture_manager->GetTextureAtlas(name);
}

TextureAtlas* GpuContext::CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name, ResourceTag tags)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	return texture_manager->CreateTextureAtlas(name, tci, sampler, tags);
}

TextureAtlas* GpuContext::CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name, ResourceTag tags)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	TextureAtlas* existing_atlas = texture_manager->GetTextureAtlas(existing_atlas_name);
	return texture_manager->CreateTextureAtlas(name, existing_atlas, sampler, tags);
}

ShaderProgramId GpuContext::InternShaderProgram(const std::string& name)
{
	return shader_manager->InternShaderProgram(name);
}

void GpuContext::CreateGraphicsPipelines()
{
	if (!shader_manager->IsDirtyGraphicsPipelines()) {
		return;
	}

	auto& shader_programs = shader_manager->ShaderPrograms();
	pipe_manager->CreateGraphicsPiplenes(shader_programs, shader_manager, pass_manager);
	shader_manager->SetDirtyGraphicsPipelines(false);
}

void GpuContext::CreateComputePipelines()
{
	if (!shader_manager->IsDirtyComputePipelines()) {
		return;
	}
	auto& compute_shader_programs = shader_manager->ComputePrograms();
	pipe_manager->CreateComputePipelines(compute_shader_programs, shader_manager);
	shader_manager->SetDirtyComputePipelines(false);
}

void GpuContext::CreateFragmentShader(const std::string& name, const char* path, const ShaderDefines& defines, ResourceTag tags) {
	shader_manager->CreateFragmentShader(name, path, defines, tags);
}

void GpuContext::CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
	const std::vector<ShaderBase::VertexSemantic>& pull, const ShaderDefines& defines, ResourceTag tags) {
	shader_manager->CreateVertexShader(name, hlsl_path, pool, pull, buffer_manager, defines, tags);
}

ShaderProgram* GpuContext::CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
	const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
	const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots, ResourceTag tags) {

	std::vector<BufferDataName> vertex_buffer_names(vertex_shader_buffers.begin(), vertex_shader_buffers.end());
	std::vector<BufferDataName> fragment_buffer_names(fragment_shader_buffers.begin(), fragment_shader_buffers.end());
	return shader_manager->CreateShaderProgram(name, spd, associated_pass_name, vs_name, std::move(vertex_buffer_names), fs_name, std::move(fragment_buffer_names), texture_slots, buffer_manager, tags);
}

void GpuContext::CreateComputeShader(const std::string& name, const char* hlsl_path, const ShaderDefines& defines, ResourceTag tags) {
	shader_manager->CreateComputeShader(name, hlsl_path, defines, tags);
}

ComputeShaderProgram* GpuContext::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
	std::initializer_list<BufferDataName> rw_storage_buffers,
	std::initializer_list<BufferDataName> ro_storage_buffers,
	std::initializer_list<ComputeRWTextureBindingParametr> rw_storage_textures,
	std::initializer_list<AtlasName> ro_storage_textures,
	std::initializer_list<AtlasName> texture_samplers,
	const ComputePassName& associated_compute_pass, ResourceTag tags)
{
	return shader_manager->CreateComputeShaderProgram(name, cs_name,
		rw_storage_buffers, ro_storage_buffers, rw_storage_textures, ro_storage_textures, texture_samplers,
		associated_compute_pass, buffer_manager, texture_manager, tags);
}

BufferData* GpuContext::CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour) {
	return buffer_manager->CreateBufferData(name, size, type, resize_behaviour);
}

BufferData* GpuContext::GetBufferData(BufferDataName name) {
	return buffer_manager->GetBufferData(name);
}

void GpuContext::CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn) {
	buffer_manager->CreateUpdateInstruction(name, std::move(fn), std::move(size_fn), std::move(offset_fn));
}

void GpuContext::CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn) {
	buffer_manager->CreatePrePassUpdateInstruction(name, std::move(fn), std::move(size_fn));
}

void GpuContext::CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn) {
	buffer_manager->CreateReadBackInstruction(name, std::move(fn), std::move(size_fn));
}

void GpuContext::CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn) {
	buffer_manager->CreatePostReadbackUpdateInstruction(name, std::move(fn), std::move(size_fn));
}
