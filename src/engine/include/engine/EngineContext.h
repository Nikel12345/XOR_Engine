#pragma once
#include "ComponentStorage.h"
struct DrawComponent;
struct ModelComponent;
#include <SDL3/SDL_gpu.h>
#include <functional>
#include <vector>
#include <string>
#include <utility>
#include <initializer_list>
#include "Aliases.h"
#include "ResourceTags.h"
#include "ObjectManager.h"
#include "BatchBuilder.h"
#include "ShaderTypes.h"
#include "ParamsSpec.h"
#include "TextureData.h"
#include "ModelData.h"
#include "GeometryPool.h"
#include "GpuContext.h"

class BufferManager;
class TextureManager;
class PassManager;
class MaterialManager;
class ShaderManager;
class ModelManager;
class CameraManager;
class PipeManager;
struct ShaderProgram;
struct ComputeShaderProgram;
class InputManager;
class TextureLoader;
class FontManager;
struct FontData;
class UI_Yoga;
class Engine;
struct GraphicsConfig;

inline constexpr const char* kScenesRoot = "saved_scene";

class EngineContext {
public:
	EngineContext(BufferManager* bm, TextureManager* tm, PassManager* pass, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* pipe, BatchBuilder* bb, TextureLoader* tl);
	~EngineContext();

	// Единственный вход в GPU-ярус снаружи: через него модуль (физика) получает контекст,
	// не линкуя Engine.
	GpuContext* GetGpuContext() { return gpu_ctx; }

	TextureAtlas* CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name, ResourceTag tags = ResourceTag::None);
	TextureAtlas* CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name, ResourceTag tags = ResourceTag::None);
	TextureHandle* CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path, ChannelConvention conv = ChannelConvention::AsIs, ResourceTag tags = ResourceTag::None);
	TextureHandle* CreateCubeMapTexture(const TextureName& name, const AtlasName& atlas_name, const char* path, ResourceTag tags = ResourceTag::None);
	TextureAtlas* GetTextureAtlas(const AtlasName& name) const;

	Material* CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, std::vector<TextureName>>> textures, std::initializer_list<ShaderName> shaders, ResourceTag tags = ResourceTag::None);

	// T обязан совпадать с раскладкой cbuffer MaterialBlock ИМЕННО ЭТОЙ sp.
	template<class T>
	void SetMaterialParams(Material* m, const ShaderName& sp_name, const T& p) { ::SetMaterialParams(m, InternShaderProgram(sp_name), sp_name, p); }
	ShaderProgramId InternShaderProgram(const std::string& name);

	// variant == 0 УБИРАЕТ запись: states разреженные, и «сбросить в дефолт» здесь то же самое,
	// что «записи нет».
	void SetEntityTextureVariant(Entity e, uint32_t mat_index, TextureSlotRole role, uint32_t variant);

	// Записью в компонент не заменяется: вместе с моделью меняются длина
	// MaterialComponent::materials и место сущности в дереве батчей.
	void ChangeModel(Entity e, const ModelName& model_name);

	void ChangeMaterial(Entity e, const MaterialName& material_name, uint32_t submesh = 0);

	FontData* CreateFont(const std::string& name, const char* path, float px, bool sdf = false);

	GeometryPool* CreateGeometryPool(const std::string& name, uint32_t vertex_size,
		const std::vector<GeometryPool::StreamDesc>& streams);

	ModelData* CreateModel(const ModelName& name, const char* model_path, const char* index_path,
		AnchorShift anchor = AnchorShift::Keep, const std::string& pool_name = {});
	ModelData* CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor = AnchorShift::Keep,
		ResourceTag tags = ResourceTag::None, const std::string& pool_name = {});

	// V обязан соответствовать раскладке пула: грубое несовпадение ловит проверка кратности.
	template<typename V, typename Fn>
	ModelData* CreateModel(const ModelName& name, Fn&& generator, AnchorShift anchor = AnchorShift::Keep,
		ResourceTag tags = ResourceTag::None, const std::string& pool_name = {})
	{
		return CreateModel(name, ModelGeneratorFn(
			[gen = std::forward<Fn>(generator)](std::vector<std::byte>& out, std::vector<Uint32>& indices) {
				std::vector<V> verts;
				gen(verts, indices);
				WriteVertices(out, verts);
			}), anchor, tags, pool_name);
	}

	void CreateGraphicsPipelines();
	void CreateComputePipelines();

	template<typename... Components>
	Entity CreateEntity(const std::string& scene_name, Components&&... comps) {
		constexpr bool needs_pib = contains_type_v<DrawComponent, Components...>
			&& contains_type_v<ModelComponent, Components...>;

		Entity entity = object_manager->CreateEntity(scene_name, std::forward<Components>(comps)...);

		if constexpr (needs_pib) {
			SceneData* active_scene = object_manager->GetActiveScene();
			SceneData* target_scene = object_manager->GetScene(scene_name);
			if (target_scene != nullptr && active_scene == target_scene) {
				batch_builder->QueueCreate(entity);
			}
		}

		return entity;
	}
	void DeleteEntity(const SceneName& scene_name, Entity e);
	// Строка трансформа остаётся на месте, индексы соседей не сдвигаются.
	void HideEntity(const SceneName& scene_name, Entity e, bool visible);
	void SetActiveScene(const SceneName& name);

	void SaveScene(const SceneName& scene_name, const std::string& scenes_root = kScenesRoot);
	void LoadScene(const SceneName& scene_name, const std::string& scenes_root = kScenesRoot);
	void ExecuteGenerators();
	// Сущности сносятся у названной сцены.
	void ClearScene(const SceneName& scene_name);

	// Вешается на УЖЕ созданную сцену: CreateScene → RegisterGenerator → Load наполняет и
	// запускает генераторы сам.
	void RegisterGenerator(const SceneName& scene_name, std::function<void()> generator);

	void CreateFragmentShader(const std::string& name, const char* hlsl_path, ResourceTag tags = ResourceTag::None, const ShaderDefines& defines = {});
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const std::string& pool_name,
		std::initializer_list<ShaderBase::VertexSemantic> pull, ResourceTag tags = ResourceTag::None, const ShaderDefines& defines = {});
	ShaderProgram* CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
		const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
		const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots, ResourceTag tags = ResourceTag::None);

	void CreateComputeShader(const std::string& name, const char* hlsl_path, ResourceTag tags = ResourceTag::None, const ShaderDefines& defines = {});
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name,
		const std::string& cs_name,
		std::initializer_list<BufferDataName> rw_storage_buffers,
		std::initializer_list<BufferDataName> ro_storage_buffers,
		std::initializer_list<ComputeRWTextureBindingParametr> rw_storage_textures,
		std::initializer_list<AtlasName> ro_storage_textures,
		std::initializer_list<AtlasName> texture_samplers,
		const ComputePassName& associated_compute_pass, ResourceTag tags = ResourceTag::None);

	BufferManager* GetBufferManager() const { return gpu_ctx->GetBufferManager(); }
	TextureManager* GetTextureManager() const { return texture_manager; }
	ShaderManager* GetShaderManager() const { return shader_manager; }
	ModelManager* GetModelManager() const { return model_manager; }
	PassManager* GetPassManager() const { return gpu_ctx->GetPassManager(); }
	ObjectManager* GetObjectManager() const { return object_manager; }
	CameraManager* GetCameraManager() const { return camera_manager; }
	MaterialManager* GetMaterialManager() const { return material_manager; }

	BatchBuilder* GetBatchBuilder() const { return batch_builder; }

	void SetInputManager(InputManager* im) { input_manager = im; }
	InputManager* GetInputManager() const { return input_manager; }

	void SetFontManager(FontManager* fm) { font_manager = fm; }
	FontManager* GetFontManager() const { return font_manager; }

	void SetUIYoga(UI_Yoga* y) { ui_yoga = y; }
	UI_Yoga* GetUIYoga() const { return ui_yoga; }

	void SetEngine(Engine* e) { engine = e; }

	void SetGraphicsConfig(GraphicsConfig* c) { graphics_config = c; }
	GraphicsConfig* GetGraphicsConfig() const { return graphics_config; }

private:
	TextureManager* texture_manager = nullptr;
	MaterialManager* material_manager = nullptr;
	ObjectManager* object_manager = nullptr;
	ShaderManager* shader_manager = nullptr;
	ModelManager* model_manager = nullptr;
	CameraManager* camera_manager = nullptr;

	BatchBuilder* batch_builder = nullptr;

	InputManager* input_manager = nullptr;
	TextureLoader* texture_loader = nullptr;
	FontManager* font_manager = nullptr;
	UI_Yoga* ui_yoga = nullptr;
	Engine* engine = nullptr;
	GraphicsConfig* graphics_config = nullptr;

	GpuContext* gpu_ctx = nullptr;
};
