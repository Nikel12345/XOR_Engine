#pragma once
#include "BufferManager.h"
#include "TextureManager.h"
#include "RenderManager.h"
#include "MaterialManager.h"
#include "ObjectManager.h"
#include "ShaderManager.h"
#include "ModelManager.h"
#include "CameraManager.h"
#include "BatchBuilder.h"
#include "PipeManager.h"
#include "GpuTaskContext.h"
#include "Aliases.h"
#include <functional>
#include <vector>

class InputManager;
class TextureLoader;

class EngineContext {
public:
	EngineContext(BufferManager* bm, TextureManager* tm, PassManager* pm, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* rm, BatchBuilder* bb, TextureLoader* tl);

	// Узкий GPU-фасад: им пользуются ShaderSet'ы и модуль физики.
	GpuTaskContext& Gpu() { return gpu_ctx; }
	GpuTaskContext* GetGpuContext() { return &gpu_ctx; }

	TextureAtlas* CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name);
	TextureAtlas* CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name);
	TextureHandle* CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path);
	// Грузит cube-текстуру (4×3 крест) в УЖЕ существующий cube-атлас (создаётся отдельно
	// через CreateTextureAtlas с tci.type=CUBE — характер атласа задаёт только tci). ctx здесь
	// дирижёр: проверяет совместимость (что атлас и правда куб + квадратный) и делегирует
	// нарезку/заливку 6 граней TextureLoader'у. Размер грани диктует tci атласа.
	TextureHandle* CreateCubeMapTexture(const TextureName& name, const AtlasName& atlas_name, const char* path);
	TextureAtlas* GetTextureAtlas(const AtlasName& name) const { return texture_manager->GetTextureAtlas(name); }

	Material* CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, TextureName>> textures, std::initializer_list<ShaderName> shaders);

	// Прокси к MaterialManager: упаковать per-material факторы (T = раскладка MaterialBlock) в Material::params.
	template<class T>
	void SetMaterialParams(Material* m, const T& p) { material_manager->SetMaterialParams(m, p); }

	ModelData* CreateModel(const ModelName& name, const char* model_path, const char* index_path, AnchorShift anchor = AnchorShift::Keep);
	ModelData* CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor = AnchorShift::Keep);

	void CreateGraphicsPipelines();
	void CreateComputePipelines();

	template<typename... Components>
	Entity CreateEntity(const std::string& scene_name, Components&&... comps) {
		// Рисуемость теперь определяет DrawComponent (см. BatchBuilder). Инкремент в
		// батчи имеет смысл лишь для рисуемого энтити с моделью и трансформом.
		constexpr bool needs_pib = contains_type_v<DrawComponent, Components...>
			&& contains_type_v<ModelComponent, Components...>
			&& contains_type_v<PositionProxy16, Components...>;

		Entity entity = object_manager->CreateEntity(scene_name, std::forward<Components>(comps)...);

		if constexpr (needs_pib) {
			SceneData* active_scene = object_manager->GetActiveScene();
			SceneData* target_scene = object_manager->GetScene(scene_name);
			if (target_scene != nullptr && active_scene == target_scene) {
				batch_builder->QueueCreate(entity);   // incremental add on next prepare
			}
		}

		return entity;
	}
	void DeleteEntity(const SceneName& scene_name, Entity e);
	// Toggles render visibility of a single entity WITHOUT touching ECS: writes
	// DrawComponent::visible (source of truth for full rebuilds) and queues an
	// incremental batch add/remove (QueueCreate/QueueDelete). The transform row stays,
	// neighbours' indices don't shift — it's the "remove from batches" half of
	// DeleteEntity without the entity teardown. Used for debug colliders (child entities).
	void HideEntity(const SceneName& scene_name, Entity e, bool visible);
	// Activating a scene swaps the whole entity set; force a full batch rebuild.
	void SetActiveScene(const SceneName& name);

	// Сохранение/загрузка сцены в текстовый файл. OM делает ECS-часть (текст ↔ сущности),
	// а здесь — файловый IO и то, что ECS-ядру недоступно: восстановление указателей на
	// ассеты по именам (Model→ModelManager, Material→MaterialManager) и взвод флагов
	// рендера (полная пересборка батчей), как при смене активной сцены.
	void SaveScene(const SceneName& scene_name, const std::string& path);
	void LoadScene(const SceneName& scene_name, const std::string& path);
	// Сносит ВСЁ содержимое сцены (сущности/иерархию; генераторы переживают) и помечает
	// батчи на полную пересборку. LoadScene вызывает его первым (replace-on-load).
	void ClearScene(const SceneName& scene_name);

	// Генераторы — функции, восстанавливающие ПРОИЗВОДНЫЕ сущности сцены из её авторских
	// данных (помечают их GeneratedComponent → не сериализуются). Хранятся в самой сцене
	// (SceneData::generators) и вешаются на УЖЕ созданную сцену: CreateScene → здесь
	// RegisterGenerator → потом Load наполняет и сам запускает их.
	void RegisterGenerator(const SceneName& scene_name, std::function<void()> generator);

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

	BufferManager* GetBufferManager() const { return buffer_manager; }
	TextureManager* GetTextureManager() const { return texture_manager; }
	ShaderManager* GetShaderManager() const { return shader_manager; }
	ModelManager* GetModelManager() const { return model_manager; }
	PassManager* GetPassManager() const { return pass_manager; }
	ObjectManager* GetObjectManager() const { return object_manager; }
	CameraManager* GetCameraManager() const { return camera_manager; }
	MaterialManager* GetMaterialManager() const { return material_manager; }

	BatchBuilder* GetBatchBuilder() const { return batch_builder; }
	TextureLoader* GetTextureLoader() const { return texture_loader; }

	void SetInputManager(InputManager* im) { input_manager = im; }
	InputManager* GetInputManager() const { return input_manager; }

private:
	BufferManager* buffer_manager = nullptr;
	TextureManager* texture_manager = nullptr;
	PassManager* pass_manager = nullptr;
	MaterialManager* material_manager = nullptr;
	ObjectManager* object_manager = nullptr;
	ShaderManager* shader_manager = nullptr;
	ModelManager* model_manager = nullptr;
	CameraManager* camera_manager = nullptr;
	PipeManager* pipe_manager = nullptr;

	BatchBuilder* batch_builder = nullptr;

	InputManager* input_manager = nullptr;
	TextureLoader* texture_loader = nullptr;

	GpuTaskContext gpu_ctx;
};
