#include "PCH.h"
#include "BaseComponents.h"
#include "EngineContext.h"
#include "Engine.h"
#include "TextureLoader.h"
#include "PassManager.h"
#include "EngineProfiler.h"
#include "GpuContext.h"
#include "TextureManager.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ShaderManager.h"
#include "PipeManager.h"
#include "FontManager.h"

using namespace ShaderBase;

EngineContext::EngineContext(BufferManager* bm, TextureManager* tm, PassManager* pass, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* pipe, BatchBuilder* bb, TextureLoader* tl)
{
	gpu_ctx = new GpuContext(bm, sm, pass, tm);
	this->buffer_manager = bm;
	this->texture_manager = tm;
	this->pass_manager = pass;
	this->material_manager = mm;
	this->object_manager = om;
	this->shader_manager = sm;
	this->model_manager = md;
	this->camera_manager = cm;
	this->pipe_manager = pipe;

	this->batch_builder = bb;
	this->texture_loader = tl;
}

EngineContext::~EngineContext()
{
	delete gpu_ctx;
}

ShaderProgramId EngineContext::InternShaderProgram(const std::string& name)
{
	return shader_manager->InternShaderProgram(name);
}

TextureAtlas* EngineContext::GetTextureAtlas(const AtlasName& name) const
{
	return texture_manager->GetTextureAtlas(name);
}

TextureAtlas* EngineContext::CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name, ResourceTag tags)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	return texture_manager->CreateTextureAtlas(name, tci, sampler, tags);
}

TextureAtlas* EngineContext::CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name, ResourceTag tags)
{
	auto sampler = texture_manager->GetSampler(sampler_name);
	TextureAtlas* existing_atlas = texture_manager->GetTextureAtlas(existing_atlas_name);
	return texture_manager->CreateTextureAtlas(name, existing_atlas, sampler, tags);
}

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

TextureHandle* EngineContext::CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path, ChannelConvention conv, ResourceTag tags) {
	TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
	if (!atlas) return nullptr;

	DecodedImage img = texture_loader->LoadFromFile(path, PixelFormatForGpuFormat(atlas->format));
	if (!img.ok()) {
		SDL_Log("EngineContext::CreateTextureFromFile failed to load '%s'", path);
		return nullptr;
	}

	// Сторону-степень-двойки ужимаем на 2P, чтобы след «контент + рамка» остался степенью двойки и
	// тайлился впритык. Условие обязано совпадать с условием рамки в _PlaceTask.
	const uint32_t P = atlas->padding;
	if (P > 0) {
		auto is_pot = [](uint32_t v) { return v && (v & (v - 1)) == 0; };
		const int shrink = 2 * (int)P;
		const int nw = (is_pot(img.width)  && img.width  < atlas->width  && (int)img.width  > shrink) ? (int)img.width  - shrink : (int)img.width;
		const int nh = (is_pot(img.height) && img.height < atlas->height && (int)img.height > shrink) ? (int)img.height - shrink : (int)img.height;
		if (nw != (int)img.width || nh != (int)img.height) {
			const SDL_PixelFormat fmt = PixelFormatForGpuFormat(atlas->format);
			const int bpp = SDL_BYTESPERPIXEL(fmt);
			SDL_Surface* src = SDL_CreateSurfaceFrom((int)img.width, (int)img.height, fmt, img.pixels.data(), (int)img.width * bpp);
			SDL_Surface* dst = src ? SDL_ScaleSurface(src, nw, nh, SDL_SCALEMODE_LINEAR) : nullptr;
			if (dst) {
				std::vector<std::byte> px((size_t)nw * nh * bpp);
				for (int y = 0; y < nh; ++y)
					SDL_memcpy(px.data() + (size_t)y * nw * bpp, (const std::byte*)dst->pixels + (size_t)y * dst->pitch, (size_t)nw * bpp);
				img.pixels = std::move(px);
				img.width  = (uint32_t)nw;
				img.height = (uint32_t)nh;
			} else {
				SDL_Log("EngineContext::CreateTextureFromFile: pot-fit scale failed for '%s': %s", path, SDL_GetError());
			}
			SDL_DestroySurface(dst);
			SDL_DestroySurface(src);
		}
	}

	if (conv == ChannelConvention::SmoothnessInGreen) {
		for (size_t i = 1; i < img.pixels.size(); i += 4)
			img.pixels[i] = std::byte{ static_cast<unsigned char>(255 - std::to_integer<int>(img.pixels[i])) };
	}
	else if (conv == ChannelConvention::DepthInAlpha) {
		for (size_t i = 3; i < img.pixels.size(); i += 4)
			img.pixels[i] = std::byte{ static_cast<unsigned char>(255 - std::to_integer<int>(img.pixels[i])) };
	}

	TextureHandle* h = texture_manager->CreateTexture(name, atlas, img.width, img.height, std::move(img.pixels), 1, tags);
	if (h) { h->atlas_id = texture_manager->AtlasIdOf(atlas_name); h->source_path = path; h->conv = conv; }
	return h;
}

TextureHandle* EngineContext::CreateCubeMapTexture(const TextureName& name, const AtlasName& atlas_name, const char* path, ResourceTag tags) {
	TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
	if (!atlas) return nullptr;

	if (atlas->texture_type != SDL_GPU_TEXTURETYPE_CUBE
	 && atlas->texture_type != SDL_GPU_TEXTURETYPE_CUBE_ARRAY) {
		SDL_Log("EngineContext::CreateCubeMapTexture: atlas '%s' is not a cube map (texture_type=%d)", atlas_name.c_str(), (int)atlas->texture_type);
		return nullptr;
	}
	if (atlas->width != atlas->height) {
		SDL_Log("EngineContext::CreateCubeMapTexture: cube atlas '%s' must be square (%ux%u)", atlas_name.c_str(), atlas->width, atlas->height);
		return nullptr;
	}
	if (atlas->layers < 6) {
		SDL_Log("EngineContext::CreateCubeMapTexture: cube atlas '%s' has %u layers, a cube needs 6", atlas_name.c_str(), atlas->layers);
		return nullptr;
	}

	DecodedCubeMap cube = texture_loader->LoadCubeMapFromFile(path, atlas->width, PixelFormatForGpuFormat(atlas->format));
	if (!cube.ok()) {
		SDL_Log("EngineContext::CreateCubeMapTexture: failed to decode cube faces from '%s'", path);
		return nullptr;
	}

	TextureHandle* h = texture_manager->CreateTexture(name, atlas, cube.faceSize, cube.faceSize, std::move(cube.pixels), 6, tags);
	if (h) { h->atlas_id = texture_manager->AtlasIdOf(atlas_name); h->source_path = path; }
	return h;
}

Material* EngineContext::CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, std::vector<TextureName>>> textures, std::initializer_list<ShaderName> shaders, ResourceTag tags)
{
	std::vector<std::pair<TextureSlotRole, std::vector<TextureId>>> texture_ids;
	texture_ids.reserve(textures.size());
	for (const auto& [role, names] : textures) {
		std::vector<TextureId> ids;
		ids.reserve(names.size());
		for (const TextureName& tn : names)
			ids.push_back(texture_manager ? texture_manager->InternTexture(tn) : TextureId{});
		texture_ids.emplace_back(role, std::move(ids));
	}
	std::vector<ShaderName> shader_names(shaders.begin(), shaders.end());
	std::vector<ShaderProgramId> shader_ids;
	shader_ids.reserve(shader_names.size());
	for (const ShaderName& sn : shader_names) shader_ids.push_back(shader_manager->InternShaderProgram(sn));

	// Все варианты слота обязаны лежать в ОДНОМ атласе: на слот биндится один Texture2DArray,
	// и вариант из чужого молча сэмплился бы из соседнего.
	for (const auto& [role, ids] : texture_ids) {
		const TextureAtlas* first = nullptr;
		for (TextureId tid : ids) {
			TextureHandle* h = texture_manager ? texture_manager->GetTextureHandle(tid) : nullptr;
			if (!h || !h->atlas) continue;
			if (!first) { first = h->atlas; continue; }
			if (h->atlas != first)
				SDL_Log("Material '%s': slot %d variant '%s' lives in another atlas than the default "
				        "- it cannot be switched to (one Texture2DArray is bound per slot)",
					name.c_str(), static_cast<int>(role), texture_manager->TextureNameOf(tid).c_str());
		}
	}

	for (const auto& shader_name : shader_names) {
		ShaderProgram* sp = shader_manager->GetShaderProgram(shader_name);
		if (!sp) {
			SDL_Log("EngineContext::Material '%s' references non existing shader program '%s'", name.c_str(), shader_name.c_str());
			continue;
		}
		for (const auto& required_role : sp->required_slots) {
			bool found = false;
			for (const auto& [role, tex_ids] : texture_ids)
				if (role == required_role) { found = true; break; }
			if (!found)
				SDL_Log("Material '%s': missing texture for required slot %d", name.c_str(), static_cast<int>(required_role));
		}
	}
	const std::string material_name = name;   // name уходит по move — копию держим для диагностики
	Material* m = material_manager->CreateMaterial(std::move(name), std::move(texture_ids), std::move(shader_ids), tags);
	material_manager->CollectSamplerUsage(m, texture_manager, material_name);
	return m;
}

void EngineContext::SetEntityTextureVariant(Entity e, uint32_t mat_index, TextureSlotRole role, uint32_t variant)
{
	SceneData* scene = object_manager ? object_manager->GetActiveScene() : nullptr;
	if (!scene || !object_manager->Has<MaterialComponent>(scene, e)) return;
	auto& mats = object_manager->GetComponent<MaterialComponent>(scene, e).materials;
	if (mat_index >= mats.size()) return;

	auto& st = mats[mat_index].states;
	auto it = std::find_if(st.begin(), st.end(), [role](const auto& p) { return p.first == role; });
	if (variant == 0) { if (it != st.end()) st.erase(it); }
	else if (it != st.end()) it->second = variant;
	else st.emplace_back(role, variant);
}

void EngineContext::ChangeModel(Entity e, const ModelName& model_name)
{
	SceneData* scene = object_manager ? object_manager->GetActiveScene() : nullptr;
	if (!scene || !object_manager->Has<ModelComponent>(scene, e)) return;

	object_manager->GetComponent<ModelComponent>(scene, e).model = model_manager->InternModel(model_name);

	if (object_manager->Has<MaterialComponent>(scene, e)) {
		const ModelData* model = model_manager->FindModel(model_name);
		object_manager->GetComponent<MaterialComponent>(scene, e).materials.resize(
			model ? model->submeshes.size() : 0);
	}

	// Буфер bound-сфер гейтится ревизией сущностей, а не деревом батчей.
	object_manager->BumpEntityRevision();
	batch_builder->QueueUpdate(e);
}

void EngineContext::ChangeMaterial(Entity e, const MaterialName& material_name, uint32_t submesh)
{
	SceneData* scene = object_manager ? object_manager->GetActiveScene() : nullptr;
	if (!scene || !object_manager->Has<MaterialComponent>(scene, e)) return;
	auto& mats = object_manager->GetComponent<MaterialComponent>(scene, e).materials;
	if (submesh >= mats.size()) return;

	mats[submesh].material = material_manager->InternMaterial(material_name);
	mats[submesh].states.clear();

	batch_builder->QueueUpdate(e);
}

FontData* EngineContext::CreateFont(const std::string& name, const char* path, float px, bool sdf)
{
	if (!font_manager) { SDL_Log("EngineContext::CreateFont: font_manager not set"); return nullptr; }
	return font_manager->CreateFont(texture_manager, name, path, px, sdf);
}

GeometryPool* EngineContext::CreateGeometryPool(const std::string& name, uint32_t vertex_size,
	const std::vector<GeometryPool::StreamDesc>& streams)
{
	return model_manager->CreateGeometryPool(buffer_manager, name, vertex_size, streams);
}

ModelData* EngineContext::CreateModel(const ModelName& name, const char* model_path, const char* index_path, AnchorShift anchor, const std::string& pool_name)
{
	return model_manager->CreateModel(name, model_path, index_path, anchor, model_manager->GetPool(pool_name));
}

ModelData* EngineContext::CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor, ResourceTag tags, const std::string& pool_name)
{
	return model_manager->CreateModel(name, std::move(generator), anchor, model_manager->GetPool(pool_name), tags);
}

void EngineContext::DeleteEntity(const SceneName& scene_name, Entity e)
{
	SceneData* target_scene = object_manager->GetScene(scene_name);
	if (!target_scene) return;

	// Детей сносим через ЭТОТ же метод, иначе их рендер-инстансы останутся в батче и их
	// трансформ-строки переедут на чужие объекты. Список копируем — рекурсия мутирует children.
	if (auto it = target_scene->children.find(e); it != target_scene->children.end()) {
		std::vector<Entity> kids = std::move(it->second);
		target_scene->children.erase(it);
		for (Entity c : kids) DeleteEntity(scene_name, c);
	}

	const bool needs_pib = object_manager->Has<ModelComponent>(target_scene, e)
		&& object_manager->Has<Positions>(target_scene, e);

	if (needs_pib && target_scene == object_manager->GetActiveScene()) {
		batch_builder->QueueDelete(e);
	}

	object_manager->DeleteEntity(target_scene, e);
}

void EngineContext::HideEntity(const SceneName& scene_name, Entity e, bool visible)
{
	SceneData* target_scene = object_manager->GetScene(scene_name);
	if (!target_scene) return;

	// visible живёт в DrawComponent, потому что полная пересборка перечитывает флаг оттуда.
	if (!object_manager->Has<DrawComponent>(target_scene, e)) return;
	DrawComponent& draw = object_manager->GetComponent<DrawComponent>(target_scene, e);
	if (draw.visible == visible) return;

	draw.visible = visible;

	const bool batched = object_manager->Has<ModelComponent>(target_scene, e)
		&& object_manager->Has<Positions>(target_scene, e);
	if (!batched || target_scene != object_manager->GetActiveScene()) return;

	if (visible) batch_builder->QueueCreate(e);
	else         batch_builder->QueueDelete(e);
}

void EngineContext::SetActiveScene(const SceneName& name)
{
	object_manager->SetActiveScene(name);
	batch_builder->SetDirtyBatches(true);
}

void EngineContext::RegisterGenerator(const SceneName& scene_name, std::function<void()> generator)
{
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) { SDL_Log("RegisterGenerator: scene '%s' not found (CreateScene first)", scene_name.c_str()); return; }
	scene->generators.push_back(std::move(generator));
}

void EngineContext::ClearScene(const SceneName& scene_name)
{
	const size_t mat = material_manager->ClearSceneMaterials();
	const size_t shd = shader_manager->ClearSceneShaders();
	const size_t mdl = model_manager->ClearSceneModels();
	const size_t tex = texture_manager->ClearSceneTextures();
	SDL_Log("ClearScene: wiped %zu materials, %zu shader records, %zu models, %zu textures", mat, shd, mdl, tex);

	if (SceneData* scene = object_manager->GetScene(scene_name)) scene->clear();
	batch_builder->SetDirtyBatches(true);
}

void EngineContext::SaveScene(const SceneName& scene_name, const std::string& scenes_root)
{
	if (engine) engine->SaveScene(scene_name, scenes_root);
	else SDL_Log("SaveScene: engine back-pointer not set");
}

void EngineContext::LoadScene(const SceneName& scene_name, const std::string& scenes_root)
{
	if (engine) engine->LoadScene(scene_name, scenes_root);
	else SDL_Log("LoadScene: engine back-pointer not set");
}

void EngineContext::ExecuteGenerators()
{
	auto scene = object_manager->GetActiveScene();
	if (!scene) return;
	for (auto& g : scene->generators)
		if (g) g();

	batch_builder->SetDirtyBatches(true);
}

void EngineContext::CreateGraphicsPipelines()
{
	if (!shader_manager->IsDirtyGraphicsPipelines()) {
		return;
	}
	
	auto& shader_programs = shader_manager->ShaderPrograms();
	pipe_manager->CreateGraphicsPiplenes(shader_programs, shader_manager, pass_manager);
	shader_manager->SetDirtyGraphicsPipelines(false);
}

void EngineContext::CreateComputePipelines()
{
	if (!shader_manager->IsDirtyComputePipelines()) {
		return;
	}
	auto& compute_shader_programs = shader_manager->ComputePrograms();
	pipe_manager->CreateComputePipelines(compute_shader_programs, shader_manager);
	shader_manager->SetDirtyComputePipelines(false);
}

// tags ставятся здесь, после создания: промах Get*Shader законен — компиляция могла не пройти.
void EngineContext::CreateFragmentShader(const std::string& name, const char* path, ResourceTag tags, const ShaderDefines& defines) {
	gpu_ctx->CreateFragmentShader(name, path, defines, tags);
}

void EngineContext::CreateVertexShader(const std::string& name, const char* hlsl_path, const std::string& pool_name,
	std::initializer_list<ShaderBase::VertexSemantic> pull, ResourceTag tags, const ShaderDefines& defines) {
	gpu_ctx->CreateVertexShader(name, hlsl_path, model_manager->GetPool(pool_name),
		std::vector<ShaderBase::VertexSemantic>(pull), defines, tags);
}

ShaderProgram* EngineContext::CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
	const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
	const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots, ResourceTag tags) {
	return gpu_ctx->CreateShaderProgram(name, spd, associated_pass_name, vs_name, vertex_shader_buffers, fs_name, fragment_shader_buffers, texture_slots, tags);
}

void EngineContext::CreateComputeShader(const std::string& name, const char* hlsl_path, ResourceTag tags, const ShaderDefines& defines) {
	gpu_ctx->CreateComputeShader(name, hlsl_path, defines, tags);
}

ComputeShaderProgram* EngineContext::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
	std::initializer_list<BufferDataName> rw_storage_buffers,
	std::initializer_list<BufferDataName> ro_storage_buffers,
	std::initializer_list<ComputeRWTextureBindingParametr> rw_storage_textures,
	std::initializer_list<AtlasName> ro_storage_textures,
	std::initializer_list<AtlasName> texture_samplers,
	const ComputePassName& associated_compute_pass, ResourceTag tags)
{
	return gpu_ctx->CreateComputeShaderProgram(name, cs_name, rw_storage_buffers, ro_storage_buffers, rw_storage_textures, ro_storage_textures, texture_samplers, associated_compute_pass, tags);
}
