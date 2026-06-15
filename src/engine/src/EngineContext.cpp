#include "PCH.h"
#include "EngineContext.h"

EngineContext::EngineContext(BufferManager* bm, TextureManager* tm, PassManager* rm, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* pm, BatchBuilder* bb)
{
	this->buffer_manager = bm;
	this->texture_manager = tm;
	this->pass_manager = rm;
	this->material_manager = mm;
	this->object_manager = om;
	this->shader_manager = sm;
	this->model_manager = md;
	this->camera_manager = cm;
	this->pipe_manager = pm;

	this->batch_builder = bb;
}

TextureAtlas* EngineContext::CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	return texture_manager->CreateTextureAtlas(name, tci, sampler);
}

TextureAtlas* EngineContext::CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	TextureAtlas* existing_atlas = texture_manager->GetTextureAtlas(existing_atlas_name);
	return texture_manager->CreateTextureAtlas(name, existing_atlas, sampler);
}

TextureHandle* EngineContext::CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path) {
	return texture_manager->CreateTextureFromFile(name, atlas_name, path);
}


Material* EngineContext::CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, TextureName>> textures, std::initializer_list<ShaderName> shaders)
{
	std::vector<ShaderProgram*> shader_programs;
	shader_programs.reserve(shaders.size());
	for (const auto& shader_name : shaders) {
		ShaderProgram* sp = shader_manager->GetShaderProgram(shader_name);
		if (!sp) {
			SDL_Log("EngineContext::Creating material with non existing shader program");
			continue;
		}
		shader_programs.push_back(sp);
	}
	std::vector<std::pair<TextureSlotRole, TextureHandle*>> texture_handles;
	texture_handles.reserve(textures.size());
	for (const auto& [role, texture_name] : textures) {
		TextureHandle* handle = texture_manager->GetTextureHandle(texture_name);
		if (!handle) {
			SDL_Log("EngineContext::Creating material with non existing texture '%s'", texture_name.c_str());
			continue;
		}
		texture_handles.emplace_back(role, handle);
	}
	return material_manager->CreateMaterial(name, texture_handles, shader_programs);
}

ModelData* EngineContext::CreateModel(const ModelName& name, const char* model_path, const char* index_path)
{
	return model_manager->CreateModel(name, model_path, index_path);
}

void EngineContext::DeleteEntity(const SceneName& scene_name, Entity e)
{
	SceneData* target_scene = object_manager->GetScene(scene_name);
	if (!target_scene) return;

	const bool needs_pib = object_manager->Has<ModelComponent>(target_scene, e)
		&& object_manager->Has<Positions>(target_scene, e);

	// Only the active scene feeds the batch tree, so only its deletions need an
	// incremental batch update.
	if (needs_pib && target_scene == object_manager->GetActiveScene()) {
		batch_builder->QueueDelete(e);   // incremental remove on next prepare
	}

	object_manager->DeleteEntity(target_scene, e);
}

void EngineContext::SetActiveScene(const SceneName& name)
{
	object_manager->SetSceneState(name, true);
	batch_builder->SetDirtyBatches(true);
}

void EngineContext::CreateGraphicsPipelines()
{
	if (!shader_manager->IsDirtyGraphicsPipelines()) {
		return;
	}
	
	auto& shader_programs = shader_manager->GetShaderPrograms();
	pipe_manager->CreateGraphicsPiplenes(shader_programs);
	shader_manager->SetDirtyGraphicsPipelines(false);
}

void EngineContext::CreateComputePipelines()
{
	if (!shader_manager->IsDirtyComputePipelines()) {
		return;
	}
	auto& compute_shader_programs = shader_manager->GetComputeShaderPrograms();
	pipe_manager->CreateComputePipelines(compute_shader_programs);
	shader_manager->SetDirtyComputePipelines(false);
}

FragmentShaderData EngineContext::CreateFragmentShader(const char* path) {
	return shader_manager->CreateFragmentShader(path);
}

VertexShaderData EngineContext::CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> vertex_buffer_layout) {
	return shader_manager->CreateVertexShader(hlsl_path, vertex_buffer_layout);
}

ShaderProgramDescription* EngineContext::CreateShaderProgramDescription(const std::string& name) {
	return shader_manager->CreateShaderProgramDescription(name);
}

ShaderProgram* EngineContext::CreateShaderProgram(const std::string& name, ShaderProgramDescription* spd, const RenderPassName& associated_pass_name,
	VertexShaderData vs, std::initializer_list<BufferDataName> vertex_shader_buffers,
	FragmentShaderData fs, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots) {

	std::vector<BufferData*> vertex_buffers;
	vertex_buffers.reserve(vertex_shader_buffers.size());
	for (const auto& buffer_name : vertex_shader_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("EngineContext::Creating shader program with non existing vertex shader buffer '%s'", buffer_name);
			continue;
		}
		vertex_buffers.push_back(bd);
	}
	std::vector<BufferData*> fragment_buffers;
	fragment_buffers.reserve(fragment_shader_buffers.size());
	for (const auto& buffer_name : fragment_shader_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("EngineContext::Creating shader program with non existing fragment shader buffer '%s'", buffer_name);
			continue;
		}
		fragment_buffers.push_back(bd);
	}
	RenderPassStep* associated_pass = pass_manager->GetRenderPassStep(associated_pass_name);
	return shader_manager->CreateShaderProgram(name, spd, associated_pass, vs, std::move(vertex_buffers), fs, std::move(fragment_buffers), texture_slots);
}

ComputeShaderData EngineContext::CreateComputeShader(const char* hlsl_path) {
	return shader_manager->CreateComputeShader(hlsl_path);
}

ComputeShaderProgram* EngineContext::CreateComputeShaderProgram(const std::string& name, ComputeShaderData cs, 
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
			SDL_Log("EngineContext::Creating compute shader program with non existing RW storage buffer '%s'", buffer_name);
			continue;
		}
		rw_buffers.push_back(bd);
	}
	std::vector<BufferData*> ro_buffers;
	ro_buffers.reserve(ro_storage_buffers.size());
	for (const auto& buffer_name : ro_storage_buffers) {
		BufferData* bd = buffer_manager->GetBufferData(buffer_name);
		if (!bd) {
			SDL_Log("EngineContext::Creating compute shader program with non existing RO storage buffer '%s'", buffer_name);
			continue;
		}
		ro_buffers.push_back(bd);
	}
	std::vector<ComputeShaderProgram::ComputeRWTextureBinding> rw_textures;
	rw_textures.reserve(rw_storage_textures.size());

	for (const auto& binding : rw_storage_textures) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(binding.texture_atlas);
		if (!atlas) {
			SDL_Log("EngineContext::Creating compute shader program with non existing RW storage texture atlas '%s'", binding.texture_atlas.c_str());
			continue;
		}
		rw_textures.push_back({ atlas, binding.mip_level, binding.layer });
	}
	std::vector<TextureAtlas*> ro_texture_atlases;
	ro_texture_atlases.reserve(ro_storage_textures.size());
	for (const auto& atlas_name : ro_storage_textures) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
		if (!atlas) {
			SDL_Log("EngineContext::Creating compute shader program with non existing RO storage texture atlas '%s'", atlas_name.c_str());
			continue;
		}
		ro_texture_atlases.push_back(atlas);
	}
	std::vector<TextureAtlas*> samplers;
	samplers.reserve(texture_samplers.size());
	for (const auto& atlas_name : texture_samplers) {
		TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
		if (!atlas) {
			SDL_Log("EngineContext::Creating compute shader program with non existing texture sampler atlas '%s'", atlas_name.c_str());
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
	SDL_Log("EngineContext::Creating compute shader program with non existing associated compute pass '%s'", associated_compute_pass.c_str());
	return nullptr;
}
