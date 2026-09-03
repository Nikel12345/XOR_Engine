#pragma once
#include "ComponentStorage.h"
// Только по имени: CreateEntity сравнивает типы через contains_type_v, полные
// определения нужны вызывающему, а не этому заголовку.
struct DrawComponent;
struct ModelComponent;
#include <SDL3/SDL_gpu.h>
#include <functional>
#include <vector>
#include <string>
#include <utility>
#include <initializer_list>
#include "Aliases.h"
#include "ObjectManager.h"
#include "BatchBuilder.h"
#include "ShaderTypes.h"
#include "ParamsSpec.h"
#include "TextureData.h"
#include "ModelData.h"
#include "GeometryPool.h"   // StreamDesc в сигнатуре CreateGeometryPool — нужен полный тип

// Менеджеры — ТОЛЬКО forward: контекст держит их указателями, а сигнатуры фасада отдают
// указатели/принимают имена. Полные заголовки инклюдит тот cpp, который реально зовёт
// методы менеджера (ctx->GetXxxManager()->...). Так правка одного менеджера не пересобирает
// всех потребителей фасада (раньше EngineContext.h был хабом на все менеджеры разом).
class BufferManager;
class TextureManager;
class PassManager;
class MaterialManager;
class ShaderManager;
class ModelManager;
class CameraManager;
class PipeManager;
class GpuTaskContext;
struct ShaderProgram;          // тяжёлая половина (ShaderData.h) — только у реальных потребителей
struct ComputeShaderProgram;
class InputManager;
class TextureLoader;
class FontManager;
struct FontData;               // возвращаем указателем (как TextureHandle*/Material*)
class UI_Yoga;                 // держим указателем (создаёт Engine, садит сеттером)
class Engine;

// Корень сцен в рабочей папке игры — ПАПКА С ПАПКАМИ. Каждая подпапка — одна сцена, и её ИМЯ
// есть имя сцены: Save/LoadScene складывают путь как scenes_root + "/" + scene_name, а список
// сцен в редакторе — просто перечисление этого каталога. Литерал один на всех: разъедься UI и
// загрузчик по разным корням — кнопка грузила бы не то, что показывает.
inline constexpr const char* kScenesRoot = "saved_scene";

class EngineContext {
public:
	EngineContext(BufferManager* bm, TextureManager* tm, PassManager* pm, MaterialManager* mm, ObjectManager* om, ShaderManager* sm, ModelManager* md, CameraManager* cm, PipeManager* rm, BatchBuilder* bb, TextureLoader* tl);
	~EngineContext();   // delete gpu_ctx (тип неполный в заголовке)

	// Узкий GPU-фасад: им пользуются ShaderSet'ы и модуль физики. Хранится указателем,
	// чтобы заголовок не тянул GpuTaskContext.h (deref неполного типа под ссылку легален).
	GpuTaskContext& Gpu() { return *gpu_ctx; }
	GpuTaskContext* GetGpuContext() { return gpu_ctx; }

	TextureAtlas* CreateTextureAtlas(const AtlasName& name, SDL_GPUTextureCreateInfo tci, const std::string& sampler_name);
	TextureAtlas* CreateTextureAtlas(const AtlasName& name, const AtlasName& existing_atlas_name, const std::string& sampler_name);
	// conv — конвенция исходного файла; импорт нормализует её к канону движка (G = roughness).
	// По умолчанию AsIs (поведение без изменений). SmoothnessInGreen инвертирует G на загрузке.
	// dont_save=true — движковый дефолт, в файл сцены не пишется (см. TextureHandle::dont_save).
	TextureHandle* CreateTextureFromFile(const TextureName& name, const AtlasName& atlas_name, const char* path, ChannelConvention conv = ChannelConvention::AsIs, bool dont_save = false);
	// Грузит cube-текстуру (4×3 крест) в УЖЕ существующий cube-атлас (создаётся отдельно
	// через CreateTextureAtlas с tci.type=CUBE — характер атласа задаёт только tci). ctx здесь
	// дирижёр: проверяет совместимость (что атлас и правда куб + квадратный) и делегирует
	// нарезку/заливку 6 граней TextureLoader'у. Размер грани диктует tci атласа.
	TextureHandle* CreateCubeMapTexture(const TextureName& name, const AtlasName& atlas_name, const char* path);
	TextureAtlas* GetTextureAtlas(const AtlasName& name) const;

	// dont_save=true — кодовая инфраструктура (напр. debug_collider), в materials.json не пишется.
	// Слот — СПИСОК имён: { { TextureSlotRole::Albedo, { "wood", "wood_cracked" } }, ... }.
	// [0] рисуется по умолчанию, остальные — варианты, переключаемые состоянием на сущности.
	Material* CreateMaterial(std::string name, std::initializer_list<std::pair<TextureSlotRole, std::vector<TextureName>>> textures, std::initializer_list<ShaderName> shaders, bool dont_save = false);

	// Тип-безопасная упаковка per-sp факторов (T = раскладка cbuffer MaterialBlock ЭТОЙ sp).
	// Адресат данных — программа: у материала на каждую sp своя ячейка (см. SpBinding).
	// Реализация одна на всех — свободный ::SetMaterialParams из ParamsSpec.h (он же
	// проставляет имя типа из реестра); фасад лишь пробрасывает, не завися от MaterialManager.h.
	template<class T>
	void SetMaterialParams(Material* m, const ShaderName& sp_name, const T& p) { ::SetMaterialParams(m, sp_name, p); }

	// Какой вариант слот-роли показывает сущность (MaterialRef::states). Правка поля НА МЕСТЕ:
	// архетип не меняется, дерево батчей не трогается вовсе — в этом вся идея переключаемых
	// текстур. variant == 0 УБИРАЕТ запись (states разреженные, и сущность уходит из буфера
	// состояний целиком), поэтому «сбросить в дефолт» и «нет записи» — одно и то же.
	// ЗВАТЬ ТОЛЬКО С SIM-ПОТОКА. С UI-потока — командой SetEntityTextureVariant, её обработчик
	// зовёт этот же метод (одна логика на оба входа).
	void SetEntityTextureVariant(Entity e, uint32_t mat_index, TextureSlotRole role, uint32_t variant);

	// Кроссменеджерская операция: растеризовать шрифт. Живёт на контексте (менеджеры не владеют
	// друг другом — см. CLAUDE.md), передаёт TextureManager/BufferManager параметрами в FontManager.
	FontData* CreateFont(const std::string& name, const char* path, float px, bool sdf = false);

	// Кроссменеджерская операция: развернуть раскладку вершин в рабочий пул. Живёт на контексте,
	// потому что трогает и ModelManager (реестр пулов, стейджинг), и BufferManager (буферы стримов
	// + инструкции заливки) — второй уходит ПАРАМЕТРОМ, см. CLAUDE.md. Зовётся на инициализации,
	// ДО создания вершинных шейдеров: те объявляют usage, по которому буферы и бейкаются.
	GeometryPool* CreateGeometryPool(const std::string& name, uint32_t vertex_size,
		const std::vector<GeometryPool::StreamDesc>& streams);

	// pool_name пусто = дефолтный пул (первый созданный) — старые сцены и код не мигрируются.
	ModelData* CreateModel(const ModelName& name, const char* model_path, const char* index_path,
		AnchorShift anchor = AnchorShift::Keep, const std::string& pool_name = {});
	// dont_save=true — движковая/кодовая процедурная модель (sphere/quad/cubes), в models.json не пишется.
	// Форма со СТЁРТЫМ типом: вершины — байты в раскладке пула. Прямо её зовут редко, обычно берут
	// типизированную обёртку ниже.
	ModelData* CreateModel(const ModelName& name, ModelGeneratorFn generator, AnchorShift anchor = AnchorShift::Keep,
		bool dont_save = false, const std::string& pool_name = {});

	// Типизированная форма: V — структура вершины, которую заполняет генератор. Задаётся ЯВНО, как
	// T у ShaderManager::CreatePushInstruction<T>, и стирание типа делает обёртка — генератор просто
	// заполняет свой вектор и про байты не знает. Раскладку по-прежнему определяет ПУЛ; V обязан ей
	// соответствовать (грубое несовпадение ловит проверка кратности в ModelManager).
	template<typename V, typename Fn>
	ModelData* CreateModel(const ModelName& name, Fn&& generator, AnchorShift anchor = AnchorShift::Keep,
		bool dont_save = false, const std::string& pool_name = {})
	{
		return CreateModel(name, ModelGeneratorFn(
			[gen = std::forward<Fn>(generator)](std::vector<std::byte>& out, std::vector<Uint32>& indices) {
				std::vector<V> verts;
				gen(verts, indices);
				WriteVertices(out, verts);
			}), anchor, dont_save, pool_name);
	}

	void CreateGraphicsPipelines();
	void CreateComputePipelines();

	template<typename... Components>
	Entity CreateEntity(const std::string& scene_name, Components&&... comps) {
		// Рисуемость теперь определяет DrawComponent (см. BatchBuilder). Positions НЕ требуется:
		// transformless-дровабл (скайбокс) батчится с PIB=-1, позицию строит его VS.
		constexpr bool needs_pib = contains_type_v<DrawComponent, Components...>
			&& contains_type_v<ModelComponent, Components...>;

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

	// Сохранение/загрузка СЦЕНЫ-ПАПКИ: scene.json (ECS) + файлы ресурсов рядом (по менеджерам,
	// подключаются поэтапно). scenes_root — КОРЕНЬ (см. kScenesRoot), сама папка сцены —
	// scenes_root/scene_name. Тонкий прокси в Engine::Save/LoadScene — оркестрация по
	// менеджерам (tm/mm/sm + om) живёт там; ctx лишь публичная точка входа.
	void SaveScene(const SceneName& scene_name, const std::string& scenes_root = kScenesRoot);
	void LoadScene(const SceneName& scene_name, const std::string& scenes_root = kScenesRoot);
	void ExecuteGenerators();
	// Сносит ВСЁ содержимое сцены (сущности/иерархию; генераторы переживают) и помечает
	// батчи на полную пересборку. LoadScene вызывает его первым (replace-on-load).
	void ClearScene(const SceneName& scene_name);

	// Генераторы — функции, восстанавливающие ПРОИЗВОДНЫЕ сущности сцены из её авторских
	// данных (помечают их GeneratedComponent → не сериализуются). Хранятся в самой сцене
	// (SceneData::generators) и вешаются на УЖЕ созданную сцену: CreateScene → здесь
	// RegisterGenerator → потом Load наполняет и сам запускает их.
	void RegisterGenerator(const SceneName& scene_name, std::function<void()> generator);

	// Create*Shader регистрируют шейдер-данные по имени в ShaderManager; CreateShaderProgram
	// ссылается на них по имени (vs_name/fs_name/cs_name). dont_save=true — движковый дефолт
	// (весь набор Engine::InitDefaultShaders), в shaders.json не пишется (см. *ShaderData::dont_save).
	void CreateFragmentShader(const std::string& name, const char* hlsl_path, bool dont_save = false, const ShaderDefines& defines = {});
	// Вершинник называет ПУЛ (по имени, как модели) и потребляемые СЕМАНТИКИ; набор и порядок
	// слотов выводит сам пул. Пустое имя пула = дефолтный.
	void CreateVertexShader(const std::string& name, const char* hlsl_path, const std::string& pool_name,
		std::initializer_list<ShaderBase::VertexSemantic> pull, bool dont_save = false, const ShaderDefines& defines = {});
	ShaderProgram* CreateShaderProgram(const std::string& name, const ShaderProgramDescription& spd, const RenderPassName& associated_pass_name,
		const std::string& vs_name, std::initializer_list<BufferDataName> vertex_shader_buffers,
		const std::string& fs_name, std::initializer_list<BufferDataName> fragment_shader_buffers,
		std::initializer_list<TextureSlotRole> texture_slots, bool dont_save = false);

	void CreateComputeShader(const std::string& name, const char* hlsl_path, bool dont_save = false, const ShaderDefines& defines = {});
	ComputeShaderProgram* CreateComputeShaderProgram(const std::string& name,
		const std::string& cs_name,
		std::initializer_list<BufferDataName> rw_storage_buffers,
		std::initializer_list<BufferDataName> ro_storage_buffers,
		std::initializer_list<ComputeRWTextureBindingParametr> rw_storage_textures,   // топ-левел тип (ShaderTypes.h)
		std::initializer_list<AtlasName> ro_storage_textures,
		std::initializer_list<AtlasName> texture_samplers,
		const ComputePassName& associated_compute_pass, bool dont_save = false);

	BufferManager* GetBufferManager() const { return buffer_manager; }
	TextureManager* GetTextureManager() const { return texture_manager; }
	ShaderManager* GetShaderManager() const { return shader_manager; }
	ModelManager* GetModelManager() const { return model_manager; }
	PassManager* GetPassManager() const { return pass_manager; }
	ObjectManager* GetObjectManager() const { return object_manager; }
	CameraManager* GetCameraManager() const { return camera_manager; }
	MaterialManager* GetMaterialManager() const { return material_manager; }
	PipeManager* GetPipeManager() const { return pipe_manager; }

	BatchBuilder* GetBatchBuilder() const { return batch_builder; }
	TextureLoader* GetTextureLoader() const { return texture_loader; }

	void SetInputManager(InputManager* im) { input_manager = im; }
	InputManager* GetInputManager() const { return input_manager; }

	// FontManager создаётся Engine и садится сюда сеттером (как InputManager) — контекст его
	// не создаёт, только держит для кроссменеджерского CreateFont.
	void SetFontManager(FontManager* fm) { font_manager = fm; }
	FontManager* GetFontManager() const { return font_manager; }

	// UI_Yoga создаётся Engine и садится сюда сеттером (как FontManager). Игра берёт его отсюда,
	// чтобы декларативно строить UI-дерево; сам Yoga наружу не течёт (см. UI_Yoga.h).
	void SetUIYoga(UI_Yoga* y) { ui_yoga = y; }
	UI_Yoga* GetUIYoga() const { return ui_yoga; }

	// Бэк-поинтер на Engine — только для делегирования Save/LoadScene (оркестрация по менеджерам).
	void SetEngine(Engine* e) { engine = e; }

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
	FontManager* font_manager = nullptr;   // см. SetFontManager (создаёт Engine)
	UI_Yoga* ui_yoga = nullptr;   // см. SetUIYoga (создаёт Engine)
	Engine* engine = nullptr;   // см. SetEngine

	GpuTaskContext* gpu_ctx = nullptr;   // указателем: заголовок не тянет GpuTaskContext.h
};
