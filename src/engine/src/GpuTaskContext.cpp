#include "PCH.h"
#include "GpuTaskContext.h"
#include "BufferManager.h"
#include "ShaderManager.h"
#include "RenderManager.h"
#include "TextureManager.h"

using namespace ShaderBase;

GpuTaskContext::GpuTaskContext(BufferManager* bm, ShaderManager* sm, PassManager* pm, TextureManager* tm)
	: buffer_manager(bm), shader_manager(sm), pass_manager(pm), texture_manager(tm)
{
}

void GpuTaskContext::CreateFragmentShader(const std::string& name, const char* path) {
	shader_manager->CreateFragmentShader(name, path);
}

void GpuTaskContext::CreateVertexShader(const std::string& name, const char* hlsl_path, const GeometryPool* pool,
	const std::vector<ShaderBase::VertexSemantic>& pull) {
	// buffer_manager — сбор usage-флагов (VERTEX выбранным стримам, INDEX индексному буферу пула).
	shader_manager->CreateVertexShader(name, hlsl_path, pool, pull, buffer_manager);
}

ShaderProgram* GpuTaskContext::CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
	const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
	const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots) {

	// Буферы sp — ССЫЛКИ ПО ИМЕНИ (BufferDataName, как vs/fs): храним сами ключи реестра, резолв
	// в BufferData* отложен на сборку батча (BatchBuilder). Существование здесь не проверяем.
	std::vector<BufferDataName> vertex_buffer_names(vertex_shader_buffers.begin(), vertex_shader_buffers.end());
	std::vector<BufferDataName> fragment_buffer_names(fragment_shader_buffers.begin(), fragment_shader_buffers.end());
	RenderPassStep* associated_pass = pass_manager->GetRenderPassStep(associated_pass_name);
	// buffer_manager — только чтобы sp записал GRAPHICS_STORAGE_READ в обёртки своих буферов
	// (ShaderManager чужих менеджеров не хранит, получает на вызове).
	return shader_manager->CreateShaderProgram(name, spd, associated_pass, vs_name, std::move(vertex_buffer_names), fs_name, std::move(fragment_buffer_names), texture_slots, buffer_manager);
}

void GpuTaskContext::CreateComputeShader(const std::string& name, const char* hlsl_path) {
	shader_manager->CreateComputeShader(name, hlsl_path);
}

ComputeShaderProgram* GpuTaskContext::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
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
		rw_textures.push_back({ atlas, binding.mip_level, binding.layer, binding.need_simultaneous });
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
		return shader_manager->CreateComputeShaderProgram(name, cs_name, std::move(rw_buffers), std::move(ro_buffers), std::move(rw_textures), std::move(ro_texture_atlases), std::move(samplers), associated_compute_pass_ptr);

	}
	associated_compute_pass_ptr = pass_manager->GetComputePrepassStep(associated_compute_pass);
	if (associated_compute_pass_ptr) {
		return shader_manager->CreateComputeShaderProgram(name, cs_name, std::move(rw_buffers), std::move(ro_buffers), std::move(rw_textures), std::move(ro_texture_atlases), std::move(samplers), associated_compute_pass_ptr);

	}
	SDL_Log("GpuTaskContext::Creating compute shader program with non existing associated compute pass '%s'", associated_compute_pass.c_str());
	return nullptr;
}

// --- Буферы: форвард в BufferManager ---
BufferData* GpuTaskContext::CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour) {
	return buffer_manager->CreateBufferData(name, size, type, resize_behaviour);
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
