#include "PCH.h"
#include "EngineContext.h"
#include "Engine.h"           // делегирование Save/LoadScene (оркестрация сцены-папки — в Engine)
#include "TextureLoader.h"
#include "RenderManager.h"    // PassManager (создание проходов, GetRenderPassStep)
#include "EngineProfiler.h"   // Prof::Clock/MsSince — тайминг фаз загрузки (реальны при любом ENGINE_PROFILE)
// Заголовок фасада держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "GpuTaskContext.h"
#include "TextureManager.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ShaderManager.h"
#include "PipeManager.h"

using namespace ShaderBase;   // VertexBufferBinding в сигнатурах Create*Shader

EngineContext::EngineContext(BufferManager* bm, TextureManager* tm, PassManager* rm, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* pm, BatchBuilder* bb, TextureLoader* tl)
{
	gpu_ctx = new GpuTaskContext(bm, sm, rm, tm);   // GPU-фасад над Buffer/Shader/Pass/Texture
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

EngineContext::~EngineContext()
{
	delete gpu_ctx;
}

TextureAtlas* EngineContext::GetTextureAtlas(const AtlasName& name) const
{
	return texture_manager->GetTextureAtlas(name);
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

TextureHandle* EngineContext::CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path, ChannelConvention conv, bool dont_save) {
	TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
	if (!atlas) return nullptr;   // GetTextureAtlas уже залогировал отсутствие

	DecodedImage img = texture_loader->LoadFromFile(path, PixelFormatForGpuFormat(atlas->format));
	if (!img.ok()) {
		SDL_Log("EngineContext::CreateTextureFromFile failed to load '%s'", path);
		return nullptr;
	}

	// Нормализация исходной конвенции к канону движка (G = linear roughness; A = height).
	// Инверсия канала формат-независима (G = индекс 1, A = индекс 3 и в RGBA, и в BGRA).
	// Делается один раз здесь на CPU, поэтому шейдер/материалы остаются без веток и флагов.
	if (conv == ChannelConvention::SmoothnessInGreen) {
		for (size_t i = 1; i < img.pixels.size(); i += 4)
			img.pixels[i] = std::byte{ static_cast<unsigned char>(255 - std::to_integer<int>(img.pixels[i])) };
	}
	else if (conv == ChannelConvention::DepthInAlpha) {
		for (size_t i = 3; i < img.pixels.size(); i += 4)
			img.pixels[i] = std::byte{ static_cast<unsigned char>(255 - std::to_integer<int>(img.pixels[i])) };
	}

	TextureHandle* h = texture_manager->CreateTexture(name, atlas, img.width, img.height, std::move(img.pixels));
	if (h) { h->atlas_name = atlas_name; h->source_path = path; h->conv = conv; h->dont_save = dont_save; }   // самоописание для редактора/сериализации
	return h;
}

TextureHandle* EngineContext::CreateCubeMapTexture(const TextureName& name, const AtlasName& atlas_name, const char* path) {
	TextureAtlas* atlas = texture_manager->GetTextureAtlas(atlas_name);
	if (!atlas) return nullptr;   // GetTextureAtlas уже залогировал отсутствие

	// Компатибилити-проверка — задача дирижёра: куб грузим только в cube-атлас, и он обязан
	// быть квадратным (требование GPU-кубмапа, иначе faceSize=width≠height даст битые грани).
	if (atlas->texture_type != SDL_GPU_TEXTURETYPE_CUBE) {
		SDL_Log("EngineContext::CreateCubeMapTexture: atlas '%s' is not a cube map (texture_type=%d)", atlas_name.c_str(), (int)atlas->texture_type);
		return nullptr;
	}
	if (atlas->width != atlas->height) {
		SDL_Log("EngineContext::CreateCubeMapTexture: cube atlas '%s' must be square (%ux%u)", atlas_name.c_str(), atlas->width, atlas->height);
		return nullptr;
	}

	// Loader отдаёт ГОЛЫЕ пиксели 6 граней (размер грани диктует tci атласа); превращение в
	// GPU-текстуры — задача TM: по разу на грань (имена name+"_f0".."_f5"), порядок = слои куба,
	// поэтому _BuildUploadTasks кладёт f-ю грань на слой f.
	DecodedCubeFaces cube = texture_loader->LoadCubeMapFromFile(path, atlas->width, PixelFormatForGpuFormat(atlas->format));
	if (!cube.ok()) {
		SDL_Log("EngineContext::CreateCubeMapTexture: failed to decode cube faces from '%s'", path);
		return nullptr;
	}

	TextureHandle* first = nullptr;
	for (int f = 0; f < 6; ++f) {
		TextureHandle* h = texture_manager->CreateTexture(name + "_f" + std::to_string(f), atlas, cube.faceSize, cube.faceSize, std::move(cube.faces[f]));
		if (f == 0) first = h;
	}
	// Самоописание — на f0-грани: одна запись манифеста ("cube": true) на весь куб, имя — логическое
	// (без "_f0"). Остальные грани без source_path — сами по себе в textures.json не попадают.
	if (first) { first->atlas_name = atlas_name; first->source_path = path; first->cube_name = name; }
	return first;
}

Material* EngineContext::CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, TextureName>> textures, std::initializer_list<ShaderName> shaders, bool dont_save)
{
	// Материал хранит ИМЕНА (name-based ссылки, резолв отложен на сборку батча). Здесь sp резолвим
	// лишь для авторской валидации: у каждого required_slot шейдера должна быть текстура в материале.
	// Проверка best-effort (варнинг, не отказ): текстуру могут добавить/создать позже.
	std::vector<std::pair<TextureSlotRole, TextureName>> texture_names(textures.begin(), textures.end());
	std::vector<ShaderName> shader_names(shaders.begin(), shaders.end());

	for (const auto& shader_name : shader_names) {
		ShaderProgram* sp = shader_manager->GetShaderProgram(shader_name);
		if (!sp) {
			SDL_Log("EngineContext::Material '%s' references non existing shader program '%s'", name.c_str(), shader_name.c_str());
			continue;
		}
		for (const auto& required_role : sp->required_slots) {
			bool found = false;
			for (const auto& [role, tex_name] : texture_names)
				if (role == required_role) { found = true; break; }
			if (!found)
				SDL_Log("Material '%s': missing texture for required slot %d", name.c_str(), static_cast<int>(required_role));
		}
	}
	const std::string material_name = name;   // name уходит по move — копию держим для диагностики
	Material* m = material_manager->CreateMaterial(std::move(name), std::move(texture_names), std::move(shader_names));
	if (m) m->dont_save = dont_save;
	// Слот материала = фрагментный сэмплер → атласы его текстур получают SAMPLER (сбор usage-флагов).
	material_manager->CollectSamplerUsage(m, texture_manager, material_name);
	return m;
}

ModelData* EngineContext::CreateModel(const ModelName& name, const char* model_path, const char* index_path, AnchorShift anchor)
{
	return model_manager->CreateModel(name, model_path, index_path, anchor);
}

ModelData* EngineContext::CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor, bool dont_save)
{
	ModelData* m = model_manager->CreateModel(name, std::move(generator), anchor);
	if (m) m->dont_save = dont_save;   // флаг ставим тут, не тащим в сигнатуру MM (см. ModelData::dont_save)
	return m;
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

void EngineContext::RegisterGenerator(const SceneName& scene_name, std::function<void()> generator)
{
	// Сцена должна уже существовать (паттерн «ресурс создан до использования»: CreateScene
	// раньше). Генератор живёт в самой сцене и переживает clear/перезагрузку.
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) { SDL_Log("RegisterGenerator: scene '%s' not found (CreateScene first)", scene_name.c_str()); return; }
	scene->generators.push_back(std::move(generator));
}

void EngineContext::ClearScene(const SceneName& scene_name)
{
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) return;   // нечего чистить (ещё не создана)

	// Полное удаление контента: архетипы + индексы + иерархия (next_entity_id→0).
	// Генераторы НЕ трогаем — они навешены однократно и должны пережить перезагрузку
	// (см. SceneData::clear). Батчи не правим точечно: ставим флаг полной пересборки —
	// BuildRenderBatches сам сбросит дерево, entity_slots и очереди дельт и отстроит от
	// (теперь пустой) сцены, так что висячих ссылок на удалённые сущности не останется.
	// Замок не нужен: рендер-проходы/каллинг читают пер-слотовые СЛЕПКИ, а не ECS.
	// Единственный живой читатель ECS на рендер-потоке — UI (осознанный компромисс,
	// см. Engine::RenderFunc).
	scene->clear();
	batch_builder->SetDirtyBatches(true);
}

void EngineContext::SaveScene(const SceneName& scene_name, const std::string& dir)
{
	// Тонкий прокси: оркестрация по менеджерам (om + tm/mm/sm по этапам) — в Engine.
	if (engine) engine->SaveScene(scene_name, dir);
	else SDL_Log("SaveScene: engine back-pointer not set");
}

void EngineContext::LoadScene(const SceneName& scene_name, const std::string& dir)
{
	if (engine) engine->LoadScene(scene_name, dir);
	else SDL_Log("LoadScene: engine back-pointer not set");
}

void EngineContext::ExecuteGenerators()
{
	auto scene = object_manager->GetActiveScene();
	for (auto& g : scene->generators)
		if (g) g();

	batch_builder->SetDirtyBatches(true);
}

void EngineContext::CreateGraphicsPipelines()
{
	if (!shader_manager->IsDirtyGraphicsPipelines()) {
		return;
	}
	
	auto& shader_programs = shader_manager->GetShaderPrograms();
	pipe_manager->CreateGraphicsPiplenes(shader_programs, shader_manager);
	shader_manager->SetDirtyGraphicsPipelines(false);
}

void EngineContext::CreateComputePipelines()
{
	if (!shader_manager->IsDirtyComputePipelines()) {
		return;
	}
	auto& compute_shader_programs = shader_manager->GetComputeShaderPrograms();
	pipe_manager->CreateComputePipelines(compute_shader_programs, shader_manager);
	shader_manager->SetDirtyComputePipelines(false);
}

// GPU-методы — тонкие форвардеры в gpu_ctx (реализация в GpuTaskContext.cpp).
// dont_save ставим тут через реестр (не тащим флаг в gpu_ctx/sm-сигнатуры): Create* создаёт SD в
// реестре, затем помечаем его. Get*Shader на промахе не логирует (см. ShaderManager) — компиляция
// могла не пройти, тогда просто некому ставить флаг.
void EngineContext::CreateFragmentShader(const std::string& name, const char* path, bool dont_save) {
	gpu_ctx->CreateFragmentShader(name, path);
	if (auto* d = shader_manager->GetFragmentShader(name)) d->dont_save = dont_save;
}

void EngineContext::CreateVertexShader(const std::string& name, const char* hlsl_path, std::initializer_list<const char*> vertex_buffer_names, bool dont_save) {
	gpu_ctx->CreateVertexShader(name, hlsl_path, vertex_buffer_names);
	if (auto* d = shader_manager->GetVertexShader(name)) d->dont_save = dont_save;
}

ShaderProgram* EngineContext::CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
	const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
	const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
	std::initializer_list<TextureSlotRole> texture_slots, bool dont_save) {
	ShaderProgram* sp = gpu_ctx->CreateShaderProgram(name, spd, associated_pass_name, vs_name, vertex_shader_buffers, fs_name, fragment_shader_buffers, texture_slots);
	if (sp) sp->dont_save = dont_save;
	return sp;
}

void EngineContext::CreateComputeShader(const std::string& name, const char* hlsl_path, bool dont_save) {
	gpu_ctx->CreateComputeShader(name, hlsl_path);
	if (auto* d = shader_manager->GetComputeShader(name)) d->dont_save = dont_save;
}

ComputeShaderProgram* EngineContext::CreateComputeShaderProgram(const std::string& name, const std::string& cs_name,
	std::initializer_list<BufferDataName> rw_storage_buffers,
	std::initializer_list<BufferDataName> ro_storage_buffers,
	std::initializer_list<ComputeShaderProgram::ComputeRWTextureBindingParametr> rw_storage_textures,
	std::initializer_list<AtlasName> ro_storage_textures,
	std::initializer_list<AtlasName> texture_samplers,
	const ComputePassName& associated_compute_pass)
{
	return gpu_ctx->CreateComputeShaderProgram(name, cs_name, rw_storage_buffers, ro_storage_buffers, rw_storage_textures, ro_storage_textures, texture_samplers, associated_compute_pass);
}
