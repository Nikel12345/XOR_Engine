#include "PCH.h"
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

	im.RegisterCommand(CommandId::HideEntity,
		[](EngineContext* ctx, const void* data)
		{
			// Данных-структуры нет — Entity и флаг упакованы прямо в указатель:
			// младшие 32 бита — Entity, бит 32 — visible (см. UI_ImGui::InspectEntity).
			const uintptr_t packed = reinterpret_cast<uintptr_t>(data);
			const Entity e = static_cast<Entity>(packed & 0xFFFFFFFFu);
			const bool visible = ((packed >> 32) & 0x1u) != 0u;
			ctx->HideEntity(ctx->GetObjectManager()->GetActiveSceneName(), e, visible);
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
	// сцены (ObjectManager::LoadScene), затем фиксап указателей ассетов по именам — зеркало
	// Engine::LoadScene (ECS-ядро менеджеров не знает). Незарезолвленное имя → nullptr,
	// сборщик батчей такие пропускает.
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
				for (Entity e : created) {
					if (om->Has<ModelComponent>(scene, e)) {
						ModelComponent& m = om->GetComponent<ModelComponent>(scene, e);
						m.model = m.name.empty() ? nullptr : (*ctx->GetModelManager())[m.name];
						if (!m.model && !m.name.empty())
							SDL_Log("CreateEntity: model '%s' not resolved (entity will not render)", m.name.c_str());
					}
					if (om->Has<MaterialComponent>(scene, e)) {
						MaterialComponent& mc = om->GetComponent<MaterialComponent>(scene, e);
						mc.materials.clear();
						mc.materials.reserve(mc.names.size());
						for (const auto& n : mc.names) {
							Material* mat = ctx->GetMaterialManager()->GetMaterial(n);
							if (!mat) SDL_Log("CreateEntity: material '%s' not resolved", n.c_str());
							mc.materials.push_back(mat);
						}
					}
					// ПОСЛЕ фиксапа указателей: инкремент читает model/materials на ближайшем
					// prepare. Отбор (Draw+visible, Model, Material) перепроверяет ApplyIncremental
					// сам — здесь очередь дешевле фильтра.
					if (feeds_batches) ctx->GetBatchBuilder()->QueueCreate(e);
				}
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
				m->textures[static_cast<TextureSlotRole>(c->role)] = c->texture;
				// Новый слот → его атлас сэмплится (сбор usage-флагов + проверка намерения).
				ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c->material);
			}
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Новый материал: sp "sp" (main) + дефолт-текстуры по его required_slots + дефолт-params.
	// Имя приходит из UI (уже свободное); если вдруг занято — CreateMaterial вернёт существующий.
	im.RegisterCommand(CommandId::CreateMaterial,
		[](EngineContext* ctx, const void* data)
		{
			const CreateMaterialCmd* c = static_cast<const CreateMaterialCmd*>(data);
			ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram("sp");
			std::vector<std::pair<TextureSlotRole, TextureName>> texs;
			if (sp) for (TextureSlotRole role : sp->required_slots) texs.emplace_back(role, DefaultTextureForRole(role));
			Material* m = ctx->GetMaterialManager()->CreateMaterial(c->name, std::move(texs), std::vector<ShaderName>{ "sp" });
			if (m) ctx->SetMaterialParams(m, OpaqueMaterialParams{});   // sp несёт MaterialBlock → нужен блоб
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
				for (auto& s : m->shader_programs) if (s == c->shader) { present = true; break; }
				if (!present) {
					m->shader_programs.push_back(c->shader);
					if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c->shader))
						for (TextureSlotRole role : sp->required_slots)
							if (!m->textures.count(role)) m->textures[role] = DefaultTextureForRole(role);
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
				for (size_t i = 0; i < sps.size(); ++i) if (sps[i] == c->shader) { sps.erase(sps.begin() + i); break; }
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
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
				ctx->CreateTextureFromFile(c->name, c->atlas, c->path.c_str(), static_cast<ChannelConvention>(c->conv));
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

			RenderPassStep* pass = ctx->GetPassManager()->GetRenderPassStep(c->passName);
			if (!pass && old) pass = old->associated_render_pass;   // пустой/неизвестный проход → прежний (правка)
			// Буферы — из формы, ПО ИМЕНИ (BufferDataName, как vs/fs): храним ключи, резолв на сборке батча.
			const std::vector<BufferDataName>   vbufs = c->vsBuffers;
			const std::vector<BufferDataName>   fbufs = c->fsBuffers;
			const std::vector<TextureSlotRole>  slots = c->slots;   // роли из формы (дубли отсеет CreateShaderProgram)
			const std::string vsName = !c->vsName.empty() ? c->vsName : (old ? old->vs_name : std::string());
			const std::string fsName = !c->fsName.empty() ? c->fsName : (old ? old->fs_name : std::string());
			auto push = old ? old->push_func : nullptr;   // push-константы — код-байндинг; у UI-создания их нет

			if (old) {   // правка: снять кэш пайплайна старой sp ДО её удаления (ключ кэша — sp*)
				ctx->GetPipeManager()->InvalidatePipeline(old, ctx->GetBatchBuilder()->RebuildEpoch());
				sm->DeleteShaderProgram(c->oldName);
			}
			ShaderProgram* nw = sm->CreateShaderProgram(finalName, c->spd, pass, vsName, vbufs, fsName, fbufs, slots, ctx->GetBufferManager());
			if (nw) nw->push_func = std::move(push);
			sm->SetDirtyGraphicsPipelines(true);
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
			delete c;
		});

	// Смена прохода sp — моментально (без подтверждения, как spd-тумблеры). associated_render_pass
	// = выбранный RenderPassStep*; пайплайн зависит от форматов прохода → инвалидация + пересборка.
	im.RegisterCommand(CommandId::SetShaderPass,
		[](EngineContext* ctx, const void* data)
		{
			const SetShaderPassCmd* c = static_cast<const SetShaderPassCmd*>(data);
			ShaderManager* sm = ctx->GetShaderManager();
			ShaderProgram*  sp = sm->GetShaderProgram(c->shader);
			RenderPassStep* rp = ctx->GetPassManager()->GetRenderPassStep(c->pass);
			if (sp && rp) {
				sp->associated_render_pass = rp;
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
				// UI говорит семантиками (pull) — как манифест; резолв в стримы пула здесь.
				std::vector<const char*> stream_names = PosUVNormPool::StreamsForSemantics(c->pull);
				std::vector<std::string> buf_names(stream_names.begin(), stream_names.end());
				sm->CreateVertexShader(c->name, c->path.c_str(), buf_names, ctx->GetBufferManager());
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
				sm->CreateFragmentShader(c->name, c->path.c_str());
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
				sm->CreateComputeShader(c->name, c->path.c_str());
				for (auto& csp : sm->GetComputeShaderPrograms())
					if (csp->cs_name == c->name || csp->cs_name == c->oldName)
						ctx->GetPipeManager()->InvalidateComputePipeline(csp.get(), ctx->GetBatchBuilder()->ComputeRebuildEpoch());
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
