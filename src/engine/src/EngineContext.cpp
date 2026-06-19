#include "PCH.h"
#include "EngineContext.h"
#include "TextureLoader.h"

EngineContext::EngineContext(BufferManager* bm, TextureManager* tm, PassManager* rm, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* pm, BatchBuilder* bb, TextureLoader* tl)
	: gpu_ctx(bm, sm, rm, tm)   // GPU-фасад над Buffer/Shader/Pass/Texture
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
	this->texture_loader = tl;
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

// В какой CPU-пиксельформат декодить, чтобы байты совпали с форматом GPU-атласа.
static SDL_PixelFormat PixelFormatForGpuFormat(SDL_GPUTextureFormat fmt)
{
	switch (fmt) {
	case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
	case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
		return SDL_PIXELFORMAT_BGRA32;
	case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
	case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
		return SDL_PIXELFORMAT_RGBA32;
	default:
		SDL_Log("EngineContext: no CPU pixel format mapping for GPU texture format %d, defaulting to BGRA32", (int)fmt);
		return SDL_PIXELFORMAT_BGRA32;
	}
}

TextureHandle* EngineContext::CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path) {
	TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
	if (!atlas) return nullptr;   // GetTextureAtlas уже залогировал отсутствие

	DecodedImage img = texture_loader->LoadFromFile(path, PixelFormatForGpuFormat(atlas->format));
	if (!img.ok()) {
		SDL_Log("EngineContext::CreateTextureFromFile failed to load '%s'", path);
		return nullptr;
	}
	return texture_manager->CreateTexture(name, atlas, img.width, img.height, std::move(img.pixels));
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

ModelData* EngineContext::CreateModel(const ModelName& name, const char* model_path, const char* index_path, AnchorShift anchor)
{
	return model_manager->CreateModel(name, model_path, index_path, anchor);
}

ModelData* EngineContext::CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor)
{
	return model_manager->CreateModel(name, std::move(generator), anchor);
}

void EngineContext::DeleteEntity(const SceneName& scene_name, Entity e)
{
	SceneData* target_scene = object_manager->GetScene(scene_name);
	if (!target_scene) return;

	// Каскад на детей по обратному индексу parent->children. Удаляем через ЭТОТ же
	// метод (не напрямую ObjectManager), чтобы каждый ребёнок снял и свой рендер-инстанс
	// (QueueDelete) — иначе его трансформ-строка осталась бы в батче и «переехала» бы на
	// чужой объект. Список копируем — рекурсия мутирует children. O(1) на потомка.
	if (auto it = target_scene->children.find(e); it != target_scene->children.end()) {
		std::vector<Entity> kids = std::move(it->second);
		target_scene->children.erase(it);
		for (Entity c : kids) DeleteEntity(scene_name, c);
	}

	const bool needs_pib = object_manager->Has<ModelComponent>(target_scene, e)
		&& object_manager->Has<Positions>(target_scene, e);

	// Only the active scene feeds the batch tree, so only its deletions need an
	// incremental batch update.
	if (needs_pib && target_scene == object_manager->GetActiveScene()) {
		batch_builder->QueueDelete(e);   // incremental remove on next prepare
	}

	object_manager->DeleteEntity(target_scene, e);
}

void EngineContext::HideEntity(const SceneName& scene_name, Entity e, bool visible)
{
	SceneData* target_scene = object_manager->GetScene(scene_name);
	if (!target_scene) return;

	// visible живёт в DrawComponent как источник истины для полной пересборки
	// (реактивация сцены → BuildRenderBatches перечитает флаг). Сам тоггл — это
	// «половина DeleteEntity»: только инкрементальное снятие/добавление рендер-инстанса,
	// без сноса энтити из ECS. Поэтому трансформ-строка остаётся, а render_instance_base
	// соседей не сдвигается (в отличие от удаления, где swap_remove перетряхивает индексы).
	if (!object_manager->Has<DrawComponent>(target_scene, e)) return;
	DrawComponent& draw = object_manager->GetComponent<DrawComponent>(target_scene, e);
	if (draw.visible == visible) return;   // no-op: не дёргаем очередь и ревизию батчей

	draw.visible = visible;

	// Батч-дерево кормит только активная сцена; инкремент имеет смысл лишь для рисуемого
	// энтити с моделью и трансформом (та же тройка-условие, что в DeleteEntity).
	const bool batched = object_manager->Has<ModelComponent>(target_scene, e)
		&& object_manager->Has<Positions>(target_scene, e);
	if (!batched || target_scene != object_manager->GetActiveScene()) return;

	if (visible) batch_builder->QueueCreate(e);   // показать: добавить рендер-инстанс
	else         batch_builder->QueueDelete(e);   // скрыть: снять рендер-инстанс
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

// GPU-методы — тонкие форвардеры в gpu_ctx (реализация в GpuTaskContext.cpp).
FragmentShaderData EngineContext::CreateFragmentShader(const char* path) {
	return gpu_ctx.CreateFragmentShader(path);
}

VertexShaderData EngineContext::CreateVertexShader(const char* hlsl_path, std::initializer_list<VertexBufferBinding> vertex_buffer_layout) {
	return gpu_ctx.CreateVertexShader(hlsl_path, vertex_buffer_layout);
}

ShaderProgramDescription* EngineContext::CreateShaderProgramDescription(const std::string& name) {
	return gpu_ctx.CreateShaderProgramDescription(name);
}

ShaderProgram* EngineContext::CreateShaderProgram(const std::string& name, ShaderProgramDescription* spd, const RenderPassName& associated_pass_name,
	VertexShaderData vs, std::initializer_list<BufferDataName> vertex_shader_buffers,
	FragmentShaderData fs, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots) {
	return gpu_ctx.CreateShaderProgram(name, spd, associated_pass_name, vs, vertex_shader_buffers, fs, fragment_shader_buffers, texture_slots);
}

ComputeShaderData EngineContext::CreateComputeShader(const char* hlsl_path) {
	return gpu_ctx.CreateComputeShader(hlsl_path);
}

ComputeShaderProgram* EngineContext::CreateComputeShaderProgram(const std::string& name, ComputeShaderData cs,
	std::initializer_list<BufferDataName> rw_storage_buffers,
	std::initializer_list<BufferDataName> ro_storage_buffers,
	std::initializer_list<ComputeShaderProgram::ComputeRWTextureBindingParametr> rw_storage_textures,
	std::initializer_list<AtlasName> ro_storage_textures,
	std::initializer_list<AtlasName> texture_samplers,
	const ComputePassName& associated_compute_pass)
{
	return gpu_ctx.CreateComputeShaderProgram(name, cs, rw_storage_buffers, ro_storage_buffers, rw_storage_textures, ro_storage_textures, texture_samplers, associated_compute_pass);
}
