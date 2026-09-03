#include "PCH.h"
#include "BaseComponents.h"
#include "DefaultCommandSet.h"
#include "InputManager.h"
#include "InputCommands.h"
#include "EngineContext.h"
#include "MaterialParams.h"
#include "PositionStructure.h"
// EngineContext.h держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "MaterialManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "UI_Yoga.h"

using namespace ShaderBase;   // VertexSemantic (pull в UpsertVertexShader)

// Дефолтная текстура по роли слота (движковые из Engine::InitDefaultResources). Custom* → dummy.
static const char* DefaultTextureForRole(TextureSlotRole r)
{
	switch (r) {
	case TextureSlotRole::Albedo:   return "default_albedo";
	case TextureSlotRole::Normal:   return "default_normal";
	case TextureSlotRole::ORM:      return "default_orm";
	case TextureSlotRole::Emissive: return "default_emissive";
	default:                        return "_NoTextureDummy";   // Custom* и прочее
	}
}

void DefaultCommandSet::SetEntityCommands(InputManager& im)
{
	im.RegisterCommand(CommandId::DeleteEntity,
		[](EngineContext* ctx, const void* data)
		{
			Entity e = static_cast<Entity>(reinterpret_cast<uintptr_t>(data));
			ctx->DeleteEntity(ctx->GetObjectManager()->GetActiveSceneName(), e);
		});

	// Draw.visible живой энтити: поле объявлено в схеме с .Cmd(HideEntity), поэтому правка
	// приходит общей нагрузкой полей. Запись флага И дельта в батчи — обе внутри HideEntity:
	// прямая запись с UI-потока перестроить дерево не может.
	im.RegisterCommand(CommandId::HideEntity,
		[](EngineContext* ctx, const void* data)
		{
			const FieldEditCmd* c = static_cast<const FieldEditCmd*>(data);
			ctx->HideEntity(ctx->GetObjectManager()->GetActiveSceneName(), c->entity, c->num != 0.0);
			delete c;
		});

	// Model.name живой энтити (.Cmd(SetEntityModel)). Смена модели меняет и состав батчей, и
	// число сабмешей — значит длину списка материалов: material_index сабмеша адресует именно
	// его, лишние записи не адресуются ничем, недостающие не отрисуются. Поэтому имя, длина и
	// перевешивание в дереве — одной операцией здесь, а не тремя правками из UI.
	im.RegisterCommand(CommandId::SetEntityModel,
		[](EngineContext* ctx, const void* data)
		{
			const FieldEditCmd* c = static_cast<const FieldEditCmd*>(data);
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetActiveScene();
			if (scene && om->Has<ModelComponent>(scene, c->entity)) {
				om->GetComponent<ModelComponent>(scene, c->entity).name = c->str;
				if (om->Has<MaterialComponent>(scene, c->entity)) {
					const auto& models = ctx->GetModelManager()->GetModels();
					auto it = models.find(c->str);   // через карту: Get* логировал бы промах
					om->GetComponent<MaterialComponent>(scene, c->entity).materials.resize(
						it != models.end() ? it->second->submeshes.size() : 0);
				}
				ctx->GetBatchBuilder()->QueueUpdate(c->entity);
			}
			delete c;
		});

	// Один слот Material.materials живой энтити (num = индекс сабмеша). Материал определяет sp и
	// атлас, то есть ключи шейдерного и текстурного батчей — энтити переезжает в дереве.
	im.RegisterCommand(CommandId::SetEntityMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const FieldEditCmd* c = static_cast<const FieldEditCmd*>(data);
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetActiveScene();
			if (scene && om->Has<MaterialComponent>(scene, c->entity)) {
				auto& mats = om->GetComponent<MaterialComponent>(scene, c->entity).materials;
				const size_t k = static_cast<size_t>(c->num);
				if (k < mats.size()) {
					mats[k].name = c->str;
					// Состояния адресованы РОЛЯМИ прежнего материала — у нового набор ролей свой,
					// и сохранённый номер варианта означал бы уже другую текстуру. Сбрасываем:
					// смена материала = его дефолтный вид.
					mats[k].states.clear();
					ctx->GetBatchBuilder()->QueueUpdate(c->entity);
				}
			}
			delete c;
		});

	// Правка трансформа гизмой. Продьюсер (UI) выделил SetTransformCmd на куче — здесь
	// пишем матрицу в Positions выбранной сущности и освобождаем payload. Источник —
	// мировая матрица (column-major glm от ImGuizmo); Positions хранит её row-major,
	// поэтому раскладываем поэлементно: трансляция уходит в w/d/h (как в остальном UI).
	im.RegisterCommand(CommandId::SetTransform,
		[](EngineContext* ctx, const void* data)
		{
			const SetTransformCmd* c = static_cast<const SetTransformCmd*>(data);
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetActiveScene();
			// Сущность могла быть удалена между push и исполнением — Has это отсекает.
			if (scene && om->Has<Positions>(scene, c->entity))
			{
				SoAElement<Positions> el = om->GetComponent<Positions>(scene, c->entity);
				Positions& P = el.container();
				const size_t i = el.i();
				const float* m = c->matrix;   // column-major: m[col*4 + row]
				P.x[i] = m[0]; P.y[i] = m[4]; P.z[i] = m[8];  P.w[i] = m[12];
				P.a[i] = m[1]; P.b[i] = m[5]; P.c[i] = m[9];  P.d[i] = m[13];
				P.e[i] = m[2]; P.f[i] = m[6]; P.g[i] = m[10]; P.h[i] = m[14];
				P.i[i] = m[3]; P.j[i] = m[7]; P.k[i] = m[11]; P.l[i] = m[15];
			}
			delete c;
		});

	// Смещение UI-узла (гизмо XY / кнопки Z). Правим дерево UI_Yoga (не ECS) — оно источник UI,
	// энтити пересоздаст ближайший Emit по dirty (его взводит NudgeNode). Payload на куче — удаляем.
	im.RegisterCommand(CommandId::NudgeUINode,
		[](EngineContext* ctx, const void* data)
		{
			const UINodeNudgeCmd* c = static_cast<const UINodeNudgeCmd*>(data);
			if (UI_Yoga* yg = ctx->GetUIYoga())
				yg->NudgeNode(c->node, c->ddx, c->ddy, c->ddz);
			delete c;
		});

	// Создание сущности из staging-формы: json одной сущности идёт ТЕМ ЖЕ путём, что файл
	// сцены (ObjectManager::LoadScene). Фиксапа указателей ассетов после него нет — модель и
	// материалы это ИМЕНА и в файле, и в рантайме, резолвит их BatchBuilder на сборке батчей.
	//
	// Добавление — ИНКРЕМЕНТАЛЬНОЕ (QueueCreate), как у EngineContext::CreateEntity: путь через
	// ObjectManager идёт мимо него, поэтому дельту ставим здесь руками. Новый батч (свой материал/
	// модель) инкремент заводит сам — AddEntityToBatches создаёт недостающие узлы дерева. Полная
	// пересборка тут была бы обходом ВСЕЙ сцены ради одной сущности: на тяжёлой сцене это фриз
	// (батч-дерево сносится и строится заново по 1М энтити), а даёт ровно тот же результат.
	im.RegisterCommand(CommandId::CreateEntity,
		[](EngineContext* ctx, const void* data)
		{
			const CreateEntityCmd* c = static_cast<const CreateEntityCmd*>(data);
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetScene(c->scene);
			if (scene) {
				// Дельту берёт только активная сцена — она одна кормит батч-дерево (тот же гейт,
				// что в CreateEntity/DeleteEntity: id чужой сцены совпал бы с чужим объектом).
				const bool feeds_batches = (scene == om->GetActiveScene());
				const std::vector<Entity> created = om->LoadScene(c->scene, c->json);
				// Отбор (Draw+visible, Model, Material) перепроверяет ApplyIncremental сам —
				// здесь очередь дешевле фильтра.
				for (Entity e : created)
					if (feeds_batches) ctx->GetBatchBuilder()->QueueCreate(e);
			}
			delete c;
		});
}

void DefaultCommandSet::SetSceneCommands(InputManager& im)
{
	// Save/Load сцены — в sim-потоке (мутация ECS + взвод пересборки батчей). Payload
	// (имя+путь) выделен на куче в UI, удаляем после применения.
	im.RegisterCommand(CommandId::SaveScene,
		[](EngineContext* ctx, const void* data)
		{
			const SceneIOCmd* c = static_cast<const SceneIOCmd*>(data);
			ctx->SaveScene(c->scene, c->path);
			delete c;
		});

	im.RegisterCommand(CommandId::LoadScene,
		[](EngineContext* ctx, const void* data)
		{
			const SceneIOCmd* c = static_cast<const SceneIOCmd*>(data);
			ctx->LoadScene(c->scene, c->path);
			delete c;
		});
}

void DefaultCommandSet::SetMaterialCommands(InputManager& im)
{
	// Смена текстуры слота материала — в sim-потоке: правим имя в Material::textures[role]
	// (name-based ссылка) и взводим ПЕРЕСБОРКУ батчей (в батче запечён разрешённый UVL текстуры).
	im.RegisterCommand(CommandId::SetMaterialTexture,
		[](EngineContext* ctx, const void* data)
		{
			const SetMaterialTextureCmd* c = static_cast<const SetMaterialTextureCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				// Номер варианта в слоте; 0 — дефолт. Вне диапазона (список успели укоротить
				// между кадром UI и исполнением) — тихо игнорируем: это не ошибка, а гонка.
				std::vector<TextureName>& variants = m->textures[static_cast<TextureSlotRole>(c->role)];
				if (variants.empty()) variants.emplace_back(c->texture);
				else if (c->variant < variants.size()) variants[c->variant] = c->texture;
				// Новый слот → его атлас сэмплится (сбор usage-флагов + проверка намерения).
				ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c->material);
			}
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Новый материал: sp "Lit" (главный PBR) + дефолт-текстуры по его required_slots + дефолт-params.
	// Имя приходит из UI (уже свободное); если вдруг занято — CreateMaterial вернёт существующий.
	im.RegisterCommand(CommandId::CreateMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const CreateMaterialCmd* c = static_cast<const CreateMaterialCmd*>(data);
			ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram("Lit");
			std::vector<std::pair<TextureSlotRole, std::vector<TextureName>>> texs;
			if (sp) for (TextureSlotRole role : sp->required_slots)
				texs.emplace_back(role, std::vector<TextureName>{ DefaultTextureForRole(role) });
			Material* m = ctx->GetMaterialManager()->CreateMaterial(c->name, std::move(texs), std::vector<ShaderName>{ "Lit" });
			if (m) ctx->SetMaterialParams(m, "Lit", OpaqueMaterialParams{});   // блоб адресован Lit: её MaterialBlock
			ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c->name);
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Добавить sp материалу: дописать имя (если ещё нет) + добрать дефолтами ТОЛЬКО новые роли
	// (общие с другими sp не трогаем — текстура роли шарится).
	im.RegisterCommand(CommandId::AddMaterialShader,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialShaderCmd* c = static_cast<const MaterialShaderCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				bool present = false;
				for (auto& b : m->shader_programs) if (b.sp == c->shader) { present = true; break; }
				if (!present) {
					// Ячейка без params: чем их наполнить, знает только автор шейдера — тип выбирается
					// в инспекторе (движок раскладку cbuffer не выводит и не угадывает).
					m->shader_programs.push_back(SpBinding{ c->shader, nullptr, {} });
					if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c->shader))
						for (TextureSlotRole role : sp->required_slots)
							if (!m->textures.count(role)) m->textures[role] = { DefaultTextureForRole(role) };
					ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c->material);
					ctx->GetBatchBuilder()->SetDirtyBatches(true);
				}
			}
			delete c;
		});

	// Убрать sp у материала (leftover-роли в textures не чистим — безвредны, просто не используются).
	im.RegisterCommand(CommandId::RemoveMaterialShader,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialShaderCmd* c = static_cast<const MaterialShaderCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				auto& sps = m->shader_programs;
				for (size_t i = 0; i < sps.size(); ++i) if (sps[i].sp == c->shader) {
					// Блоб НЕ освобождаем: на его адрес ещё смотрит слепок рендера (живёт несколько
					// кадров после этой правки) — переселяем на кладбище материала, см. retired_params.
					if (sps[i].params) m->retired_params.push_back(std::move(sps[i].params));
					sps.erase(sps.begin() + i);
					break;
				}
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Дописать вариант слот-роли КОПИЕЙ дефолта: новый вариант сразу резолвится (не даёт dummy),
	// а нужную текстуру ему назначат следующим SetMaterialTexture. Потолок MAX_UVL_BLOCKS здесь
	// НЕ проверяем: он на пару (материал, sp) — таблицу собирает BatchBuilder, он же и логирует
	// переполнение. UI гасит кнопку заранее, это лишь страховка от кривого вызова.
	im.RegisterCommand(CommandId::AddMaterialTextureVariant,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialVariantCmd* c = static_cast<const MaterialVariantCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				std::vector<TextureName>& variants = m->textures[static_cast<TextureSlotRole>(c->role)];
				variants.push_back(variants.empty() ? TextureName{} : variants[0]);
				ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c->material);
				// Структурная правка: сменились длина таблицы UVL и нумерация ячеек секции.
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Убрать вариант. Ноль убрать нельзя — это ДЕФОЛТ слота, то, что рисуется без переключения;
	// «убрать текстуру у слота» — другая операция, её тут нет.
	// Состояния на сущностях НЕ подрезаем: номер, ставший протухшим, гасит кламп v >= count
	// в шейдере (объект показывает дефолт). Обходить ради этого весь ECS дороже и не полнее.
	im.RegisterCommand(CommandId::RemoveMaterialTextureVariant,
		[](EngineContext* ctx, const void* data)
		{
			const MaterialVariantCmd* c = static_cast<const MaterialVariantCmd*>(data);
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c->material)) {
				auto it = m->textures.find(static_cast<TextureSlotRole>(c->role));
				if (it != m->textures.end() && c->variant > 0 && c->variant < it->second.size()) {
					it->second.erase(it->second.begin() + c->variant);
					ctx->GetBatchBuilder()->SetDirtyBatches(true);
				}
			}
			delete c;
		});

	// Какой вариант показывает энтити. НЕ структурная правка: пишем поле существующего объекта,
	// архетип и дерево батчей не трогаются — в этом вся идея фичи (два куба с одним материалом
	// показывают разное и остаются в одном инстанс-батче). Заливка подхватит со следующего кадра.
	im.RegisterCommand(CommandId::SetEntityTextureVariant,
		[](EngineContext* ctx, const void* data)
		{
			const EntityTextureVariantCmd* c = static_cast<const EntityTextureVariantCmd*>(data);
			// Вся логика — в EngineContext::SetEntityTextureVariant: тот же вход есть у игровых
			// систем с sim-потока (наведение на UI), и раздваивать её нельзя.
			ctx->SetEntityTextureVariant(c->entity, c->mat_index,
				static_cast<TextureSlotRole>(c->role), c->variant);
			delete c;
		});

	// Переименование материала — ре-кей в словаре + пересборка (материалы резолвятся по имени).
	im.RegisterCommand(CommandId::RenameMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const RenameMaterialCmd* c = static_cast<const RenameMaterialCmd*>(data);
			if (ctx->GetMaterialManager()->RenameMaterial(c->oldName, c->newName))
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});
}

void DefaultCommandSet::SetTextureCommands(InputManager& im)
{
	// Upsert текстуры — в sim-потоке: delete-if-exists + загрузка из файла (edit=create, без ветвлений).
	// Ребилд батчей: материалы, ссылающиеся на это имя, перепривяжутся к новому хэндлу.
	im.RegisterCommand(CommandId::UpsertTexture,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertTextureCmd* c = static_cast<const UpsertTextureCmd*>(data);
			if (!c->name.empty() && !c->atlas.empty() && !c->path.empty()) {
				if (!c->old_name.empty() && c->old_name != c->name) {
					ctx->GetTextureManager()->DeleteTextureHandle(c->old_name);   // переименование → снять старую
					ctx->GetTextureManager()->ReleasePreview(c->old_name);        // старого имени больше нет — превью тоже
				}
				ctx->GetTextureManager()->DeleteTextureHandle(c->name);           // replace под новым именем (no-op, если нет)
				// ReleasePreview(name) НЕ зовём: это replace того же имени, слот превью должен пережить
				// пересоздание (иначе плитка мигнёт затычкой до нового блита).
				// Куб — это ОДИН хэндл на 6 слоёв, поэтому и снятие выше, и превью, и переименование
				// работают для него теми же строками, что и для обычной текстуры: различие ровно в
				// том, каким методом читается файл.
				if (c->cube) ctx->CreateCubeMapTexture(c->name, c->atlas, c->path.c_str());
				else         ctx->CreateTextureFromFile(c->name, c->atlas, c->path.c_str(), static_cast<ChannelConvention>(c->conv));
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Удаление текстуры — снять хэндл (материалы по имени → dummy) + пересборка.
	im.RegisterCommand(CommandId::DeleteTexture,
		[](EngineContext* ctx, const void* data)
		{
			const DeleteTextureCmd* c = static_cast<const DeleteTextureCmd*>(data);
			ctx->GetTextureManager()->DeleteTextureHandle(c->name);
			ctx->GetTextureManager()->ReleasePreview(c->name);   // реальное удаление → освободить превью-ячейку
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});
}

void DefaultCommandSet::SetModelCommands(InputManager& im)
{
	// Upsert модели из файла — перезагрузка in-place (указатель у энтити жив) + пересборка батчей.
	im.RegisterCommand(CommandId::UpsertModel,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertModelCmd* c = static_cast<const UpsertModelCmd*>(data);
			if (!c->name.empty() && !c->model_path.empty() && !c->index_path.empty()) {
				ctx->GetModelManager()->LoadModelFromFile(c->name, c->model_path, c->index_path,
					static_cast<AnchorShift>(c->anchor));
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});
}

void DefaultCommandSet::SetShaderCommands(InputManager& im)
{
	// Применить правку spd шейдера: сбросить его пайплайн + взвести пересоздание пайплайнов и
	// пересборку батчей (батч кэширует указатель пайплайна). spd уже поправлен в UI in-place.
	im.RegisterCommand(CommandId::RebuildShaderPipeline,
		[](EngineContext* ctx, const void* data)
		{
			const RebuildShaderPipelineCmd* c = static_cast<const RebuildShaderPipelineCmd*>(data);
			if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c->shader)) {
				ctx->GetPipeManager()->InvalidatePipeline(sp, ctx->GetBatchBuilder()->RebuildEpoch());
				ctx->GetShaderManager()->SetDirtyGraphicsPipelines(true);   // CreateGraphicsPiplenes пересоздаст
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Удаление sp: пайплайн — в отложенное удаление, затем erase sp (шейдеры релизятся по refcount:
	// неиспользуемые освобождаются, общие живут). Материалы с этой sp → fallback (см. BatchBuilder).
	im.RegisterCommand(CommandId::DeleteShader,
		[](EngineContext* ctx, const void* data)
		{
			const RebuildShaderPipelineCmd* c = static_cast<const RebuildShaderPipelineCmd*>(data);
			if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c->shader)) {
				ctx->GetPipeManager()->InvalidatePipeline(sp, ctx->GetBatchBuilder()->RebuildEpoch());           // сперва пайплайн (кэш по sp*)
				ctx->GetShaderManager()->DeleteShaderProgram(c->shader); // затем сам sp
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// Пересоздание/СОЗДАНИЕ sp по кнопке-подтверждению (как Upsert текстуры/модели). Одна форма и
	// команда: старая sp найдена (oldName) → правка = delete+create (кэш пайплайна по sp* снести ДО);
	// не найдена (плитка «+», oldName пуст) → чистое создание. vs/fs/буферы/слоты/проход/spd — из формы.
	im.RegisterCommand(CommandId::RecreateShader,
		[](EngineContext* ctx, const void* data)
		{
			const RecreateShaderCmd* c = static_cast<const RecreateShaderCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			ShaderProgram* old = sm->GetShaderProgram(c->oldName);   // nullptr = создание с нуля

			// Имя результата. Правка: свободное новое → ренейм, иначе прежнее (ссылки материалов по
			// СТАРОМУ имени НЕ чиним — конвенция движка, на пересборке дадут fallback). Создание:
			// newName обязан быть непустым и свободным (UI гарантирует кнопкой), иначе отказ.
			std::string finalName;
			if (old)
				finalName = (!c->newName.empty() && c->newName != c->oldName
					&& !sm->GetShaderPrograms().count(c->newName)) ? c->newName : c->oldName;
			else {
				if (c->newName.empty() || sm->GetShaderPrograms().count(c->newName)) { delete c; return; }
				finalName = c->newName;
			}

			// Проход — по имени. Пустое/неизвестное имя при правке → оставляем прежнее.
			std::string passName = c->passName;
			if (!ctx->GetPassManager()->GetRenderPassStep(passName) && old) passName = old->render_pass_name;
			// Буферы — из формы, ПО ИМЕНИ (BufferDataName, как vs/fs): храним ключи, резолв на сборке батча.
			const std::vector<BufferDataName>   vbufs = c->vsBuffers;
			const std::vector<BufferDataName>   fbufs = c->fsBuffers;
			const std::vector<TextureSlotRole>  slots = c->slots;   // роли из формы (дубли отсеет CreateShaderProgram)
			const std::string vsName = !c->vsName.empty() ? c->vsName : (old ? old->vs_name : std::string());
			const std::string fsName = !c->fsName.empty() ? c->fsName : (old ? old->fs_name : std::string());

			if (old) {   // правка: снять кэш пайплайна старой sp ДО её удаления (ключ кэша — sp*)
				ctx->GetPipeManager()->InvalidatePipeline(old, ctx->GetBatchBuilder()->RebuildEpoch());
				sm->DeleteShaderProgram(c->oldName);
			}
			// push-инструкции не переносим руками: CreateShaderProgram сам возьмёт код-байндинги из
			// реестра ПО ИМЕНИ. Переименование = смена владельца функции — перенос со старого
			// имени всё равно жил бы лишь до ближайшей LoadScene, где связывает имя.
			ShaderProgram* nw = sm->CreateShaderProgram(finalName, c->spd, passName, vsName, vbufs, fsName, fbufs, slots, ctx->GetBufferManager());
			sm->SetDirtyGraphicsPipelines(true);
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Смена прохода sp — моментально (без подтверждения, как spd-тумблеры). Пишем ИМЯ из команды;
	// проход ищем только чтобы убедиться, что он есть. Пайплайн зависит от форматов прохода →
	// инвалидация + пересборка.
	im.RegisterCommand(CommandId::SetShaderPass,
		[](EngineContext* ctx, const void* data)
		{
			const SetShaderPassCmd* c = static_cast<const SetShaderPassCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			ShaderProgram*  sp = sm->GetShaderProgram(c->shader);
			const bool pass_exists = ctx->GetPassManager()->GetRenderPassStep(c->pass) != nullptr;
			if (sp && pass_exists) {
				sp->render_pass_name = c->pass;
				ctx->GetPipeManager()->InvalidatePipeline(sp, ctx->GetBatchBuilder()->RebuildEpoch());
				sm->SetDirtyGraphicsPipelines(true);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	// --- Upsert/Delete шейдер-данных из формы редактора SD. Пайплайны ссылающихся sp/csp инвалидируем. ---
	im.RegisterCommand(CommandId::UpsertVertexShader,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertVertexShaderCmd* c = static_cast<const UpsertVertexShaderCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c->name.empty() && !c->path.empty()) {
				if (!c->oldName.empty() && c->oldName != c->name) sm->DeleteVertexShader(c->oldName);
				// UI говорит пулом + семантиками — тем же языком, что манифест; стримы резолвит пул.
				sm->CreateVertexShader(c->name, c->path.c_str(), ctx->GetModelManager()->GetPool(c->pool),
					c->pull, ctx->GetBufferManager(), c->defines);
				for (auto& [sn, spp] : sm->GetShaderPrograms())   // пересобрать пайплайны sp на этом vs
					if (spp->vs_name == c->name || spp->vs_name == c->oldName)
						ctx->GetPipeManager()->InvalidatePipeline(spp.get(), ctx->GetBatchBuilder()->RebuildEpoch());
				sm->SetDirtyGraphicsPipelines(true);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	im.RegisterCommand(CommandId::UpsertFragmentShader,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertFragmentShaderCmd* c = static_cast<const UpsertFragmentShaderCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c->name.empty() && !c->path.empty()) {
				if (!c->oldName.empty() && c->oldName != c->name) sm->DeleteFragmentShader(c->oldName);
				sm->CreateFragmentShader(c->name, c->path.c_str(), c->defines);
				for (auto& [sn, spp] : sm->GetShaderPrograms())
					if (spp->fs_name == c->name || spp->fs_name == c->oldName)
						ctx->GetPipeManager()->InvalidatePipeline(spp.get(), ctx->GetBatchBuilder()->RebuildEpoch());
				sm->SetDirtyGraphicsPipelines(true);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
			delete c;
		});

	im.RegisterCommand(CommandId::UpsertComputeShader,
		[](EngineContext* ctx, const void* data)
		{
			const UpsertComputeShaderCmd* c = static_cast<const UpsertComputeShaderCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c->name.empty() && !c->path.empty()) {
				if (!c->oldName.empty() && c->oldName != c->name) sm->DeleteComputeShader(c->oldName);
				sm->CreateComputeShader(c->name, c->path.c_str(), c->defines);
				for (auto& slot : sm->GetComputeShaderPrograms())
					if (slot.program && (slot.program->cs_name == c->name || slot.program->cs_name == c->oldName))
						ctx->GetPipeManager()->InvalidateComputePipeline(slot.program.get(), ctx->GetBatchBuilder()->ComputeRebuildEpoch());
				sm->SetDirtyComputePipelines(true);
				sm->SetDirtyComputeBatches(true);
			}
			delete c;
		});

	im.RegisterCommand(CommandId::DeleteVertexShader,
		[](EngineContext* ctx, const void* data)
		{
			const ShaderDataNameCmd* c = static_cast<const ShaderDataNameCmd*>(data);
			// Используемый SD менеджер удалить откажется (пайплайн собран из его данных, fallback
			// с чужой раскладкой невозможен); неиспользуемый ничего не рисует — dirty-флаги не нужны.
			ctx->GetShaderManager()->DeleteVertexShader(c->name);
			delete c;
		});

	im.RegisterCommand(CommandId::DeleteFragmentShader,
		[](EngineContext* ctx, const void* data)
		{
			const ShaderDataNameCmd* c = static_cast<const ShaderDataNameCmd*>(data);
			ctx->GetShaderManager()->DeleteFragmentShader(c->name);   // отказ/чистое удаление — см. DeleteVertexShader
			delete c;
		});

	im.RegisterCommand(CommandId::DeleteComputeShader,
		[](EngineContext* ctx, const void* data)
		{
			const ShaderDataNameCmd* c = static_cast<const ShaderDataNameCmd*>(data);
			ctx->GetShaderManager()->DeleteComputeShader(c->name);   // отказ/чистое удаление — см. DeleteVertexShader
			delete c;
		});
}
