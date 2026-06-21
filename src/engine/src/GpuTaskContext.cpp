#include "PCH.h"
#include "GpuTaskContext.h"
#include "BufferManager.h"
#include "ShaderManager.h"
#include "RenderManager.h"
#include "TextureManager.h"

GpuTaskContext::GpuTaskContext(BufferManager* bm, ShaderManager* sm, PassManager* pm, TextureManager* tm)
	: buffer_manager(bm), shader_manager(sm), pass_manager(pm), texture_manager(tm)
{
}

FragmentShaderData GpuTaskContext::CreateFragmentShader(const char* path) {
	return shader_manager->CreateFragmentShader(path);
}

FragmentShaderData GpuTaskContext::CreateMaterialFragmentShader(const char* base_path, const char* user_path) {
	return shader_manager->CreateMaterialFragmentShader(base_path, user_path);
}

VertexShaderData GpuTaskContext::CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> vertex_buffer_layout) {
	return shader_manager->CreateVertexShader(hlsl_path, vertex_buffer_layout);
}

ShaderProgramDescription* GpuTaskContext::CreateShaderProgramDescription(const std::string& name) {
	return shader_manager->CreateShaderProgramDescription(name);
}

ShaderProgram* GpuTaskContext::CreateShaderProgram(const std::string& name, ShaderProgramDescription* spd, const RenderPassName& associated_pass_name,
	VertexShaderData vs, std::initializer_list<BufferDataName> vertex_shader_buffers,
	FragmentShaderData fs, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots) {

	std::vector<BufferData*> vertex_buffers;
	vertex_buffers.reserve(vertex_shader_buffers.size());
	for (const auto& buffer_name : vertex_shader_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("GpuTaskContext::Creating shader program with non existing vertex shader buffer '%s'", buffer_name);
			continue;
		}
		vertex_buffers.push_back(bd);
	}
	std::vector<BufferData*> fragment_buffers;
	fragment_buffers.reserve(fragment_shader_buffers.size());
	for (const auto& buffer_name : fragment_shader_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("GpuTaskContext::Creating shader program with non existing fragment shader buffer '%s'", buffer_name);
			continue;
		}
		fragment_buffers.push_back(bd);
	}
	RenderPassStep* associated_pass = pass_manager->GetRenderPassStep(associated_pass_name);
	return shader_manager->CreateShaderProgram(name, spd, associated_pass, vs, std::move(vertex_buffers), fs, std::move(fragment_buffers), texture_slots);
}

ComputeShaderData GpuTaskContext::CreateComputeShader(const char* hlsl_path) {
	return shader_manager->CreateComputeShader(hlsl_path);
}

ComputeShaderProgram* GpuTaskContext::CreateComputeShaderProgram(const std::string& name, ComputeShaderData cs,
	std::initializer_list<BufferDataName> rw_storage_buffers,
	std::initializer_list<BufferDataName> ro_storage_buffers,
	std::initializer_list<ComputeShaderProgram::ComputeRWTextureBindingParametr> rw_storage_textures,
	std::initializer_list<AtlasName> ro_storage_textures,
	std::initializer_list<AtlasName> texture_samplers,
	const ComputePassName& associated_compute_pass)
{
	std::vector<BufferData*> rw_buffers;
	rw_buffers.reserve(rw_storage_buffers.size());
	for (const auto& buffer_name : rw_storage_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("GpuTaskContext::Creating compute shader program with non existing RW storage buffer '%s'", buffer_name);
			continue;
		}
		rw_buffers.push_back(bd);
	}
	std::vector<BufferData*> ro_buffers;
	ro_buffers.reserve(ro_storage_buffers.size());
	for (const auto& buffer_name : ro_storage_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("GpuTaskContext::Creating compute shader program with non existing RO storage buffer '%s'", buffer_name);
			continue;
		}
		ro_buffers.push_back(bd);
	}
	std::vector<ComputeShaderProgram::ComputeRWTextureBinding> rw_textures;
	rw_textures.reserve(rw_storage_textures.size());

	for (const auto& binding : rw_storage_textures) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(binding.texture_atlas);
		if (!atlas) {
			SDL_Log("GpuTaskContext::Creating compute shader program with non existing RW storage texture atlas '%s'", binding.texture_atlas.c_str());
			continue;
		}
		rw_textures.push_back({ atlas, binding.mip_level, binding.layer });
	}
	std::vector<TextureAtlas*> ro_texture_atlases;
	ro_texture_atlases.reserve(ro_storage_textures.size());
	for (const auto& atlas_name : ro_storage_textures) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
		if (!atlas) {
			SDL_Log("GpuTaskContext::Creating compute shader program with non existing RO storage texture atlas '%s'", atlas_name.c_str());
			continue;
		}
		ro_texture_atlases.push_back(atlas);
	}
	std::vector<TextureAtlas*> samplers;
	samplers.reserve(texture_samplers.size());
	for (const auto& atlas_name : texture_samplers) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
		if (!atlas) {
			SDL_Log("GpuTaskContext::Creating compute shader program with non existing texture sampler atlas '%s'", atlas_name.c_str());
			continue;
		}
		samplers.push_back(atlas);
	}

	ComputePassStep* associated_compute_pass_ptr = nullptr;
	associated_compute_pass_ptr = pass_manager->GetComputePassStep(associated_compute_pass);
	if (associated_compute_pass_ptr) {
		return shader_manager->CreateComputeShaderProgram(name, cs, std::move(rw_buffers), std::move(ro_buffers), std::move(rw_textures), std::move(ro_texture_atlases), std::move(samplers), associated_compute_pass_ptr);

	}
	associated_compute_pass_ptr = pass_manager->GetComputePrepassStep(associated_compute_pass);
	if (associated_compute_pass_ptr) {
		return shader_manager->CreateComputeShaderProgram(name, cs, std::move(rw_buffers), std::move(ro_buffers), std::move(rw_textures), std::move(ro_texture_atlases), std::move(samplers), associated_compute_pass_ptr);

	}
	SDL_Log("GpuTaskContext::Creating compute shader program with non existing associated compute pass '%s'", associated_compute_pass.c_str());
	return nullptr;
}

// --- Буферы: форвард в BufferManager ---
BufferData* GpuTaskContext::CreateBufferData(BufferDataName name, Uint32 size, SDL_GPUBufferUsageFlags usage, BufferDataType type, ResizeBehaviour resize_behaviour) {
	return buffer_manager->CreateBufferData(name, size, usage, type, resize_behaviour);
}

BufferData* GpuTaskContext::GetBufferData(BufferDataName name) {
	return buffer_manager->GetBufferData(name);
}

void GpuTaskContext::CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn) {
	buffer_manager->CreateUpdateInstruction(name, std::move(fn), std::move(size_fn), std::move(offset_fn));
}

void GpuTaskContext::CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn) {
	buffer_manager->CreatePrePassUpdateInstruction(name, std::move(fn), std::move(size_fn));
}

void GpuTaskContext::CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn, ReadBackInstructionSizeFunc size_fn) {
	buffer_manager->CreateReadBackInstruction(name, std::move(fn), std::move(size_fn));
}

void GpuTaskContext::CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn) {
	buffer_manager->CreatePostReadbackUpdateInstruction(name, std::move(fn), std::move(size_fn));
}
