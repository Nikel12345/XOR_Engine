#include "PCH.h"
#include "BaseComponents.h"
#include "DefaultCommandSet.h"
#include "InputManager.h"
#include "InputCommands.h"
#include "EngineContext.h"
#include "MaterialParams.h"
#include "PositionStructure.h"
#include "MaterialManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "PipeManager.h"
#include "PassManager.h"
#include "UI_Yoga.h"

using namespace ShaderBase;

// Сами текстуры заводит Engine::InitDefaultResources.
static const char* DefaultTextureForRole(TextureSlotRole r)
{
	switch (r) {
	case TextureSlotRole::Albedo:   return "default_albedo";
	case TextureSlotRole::Normal:   return "default_normal";
	case TextureSlotRole::ORM:      return "default_orm";
	case TextureSlotRole::Emissive: return "default_emissive";
	default:                        return "NoTextureDummy";   // Custom* и прочее
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

	// Запись флага И дельта в батчи — обе внутри HideEntity: прямой записью с UI-потока дерево
	// батчей не перестроить.
	cmd::Register<CommandId::HideEntity>(im,
		[](EngineContext* ctx, const FieldEditCmd& c)
		{
			ctx->HideEntity(ctx->GetObjectManager()->GetActiveSceneName(), c.entity, c.num != 0.0);
		});

	// Смена ресурса — не запись поля: у модели меняется ещё и длина списка материалов, у материала
	// сбрасываются states, и сущность перевешивается в дереве батчей. Логика на EngineContext — её
	// же зовёт игровой код прямо с sim-потока; команда здесь только транспорт с UI.
	cmd::Register<CommandId::SetEntityModel>(im,
		[](EngineContext* ctx, const FieldEditCmd& c)
		{
			ctx->ChangeModel(c.entity, c.str);
		});

	cmd::Register<CommandId::SetEntityMaterial>(im,
		[](EngineContext* ctx, const FieldEditCmd& c)
		{
			ctx->ChangeMaterial(c.entity, c.str, safe_f_u32(static_cast<float>(c.num)));
		});

	// ImGuizmo отдаёт column-major, Positions хранит row-major — отсюда поэлементная перекладка.
	cmd::Register<CommandId::SetTransform>(im,
		[](EngineContext* ctx, const SetTransformCmd& c)
		{
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetActiveScene();
			// Сущность могла быть удалена между push и исполнением — Has это отсекает.
			if (scene && om->Has<Positions>(scene, c.entity))
			{
				SoAElement<Positions> el = om->GetComponent<Positions>(scene, c.entity);
				Positions& P = el.container();
				const size_t i = el.i();
				const float* m = c.matrix;   // column-major: m[col*4 + row]
				P.x[i] = m[0]; P.y[i] = m[4]; P.z[i] = m[8];  P.w[i] = m[12];
				P.a[i] = m[1]; P.b[i] = m[5]; P.c[i] = m[9];  P.d[i] = m[13];
				P.e[i] = m[2]; P.f[i] = m[6]; P.g[i] = m[10]; P.h[i] = m[14];
				P.i[i] = m[3]; P.j[i] = m[7]; P.k[i] = m[11]; P.l[i] = m[15];
			}
		});

	// Источник — дерево, энтити пересоздаст ближайший Emit по dirty (его взводит NudgeNode).
	cmd::Register<CommandId::NudgeUINode>(im,
		[](EngineContext* ctx, const UINodeNudgeCmd& c)
		{
			if (UI_Yoga* yg = ctx->GetUIYoga())
				yg->NudgeNode(c.node, c.ddx, c.ddy, c.ddz);
		});

	// Добавление ИНКРЕМЕНТАЛЬНОЕ: путь через ObjectManager::LoadScene идёт мимо
	// EngineContext::CreateEntity, поэтому дельту ставим руками. Полная пересборка дала бы тот же
	// результат ценой обхода ВСЕЙ сцены — на 1М энтити это фриз ради одной сущности.
	cmd::Register<CommandId::CreateEntity>(im,
		[](EngineContext* ctx, const CreateEntityCmd& c)
		{
			ObjectManager* om = ctx->GetObjectManager();
			SceneData* scene = om->GetScene(c.scene);
			if (scene) {
				// Дельту берёт только активная сцена — она одна кормит батч-дерево (id чужой
				// сцены попал бы в чужой объект).
				const bool feeds_batches = (scene == om->GetActiveScene());
				const std::vector<Entity> created = om->LoadScene(c.scene, c.json);
				for (Entity e : created)
					if (feeds_batches) ctx->GetBatchBuilder()->QueueCreate(e);
			}
		});
}

void DefaultCommandSet::SetSceneCommands(InputManager& im)
{
	cmd::Register<CommandId::SaveScene>(im,
		[](EngineContext* ctx, const SceneIOCmd& c)
		{
			ctx->SaveScene(c.scene, c.path);
		});

	cmd::Register<CommandId::LoadScene>(im,
		[](EngineContext* ctx, const SceneIOCmd& c)
		{
			ctx->LoadScene(c.scene, c.path);
		});
}

void DefaultCommandSet::SetMaterialCommands(InputManager& im)
{
	// В батче запечён уже разрешённый UVL текстуры — правка id требует ПЕРЕСБОРКИ.
	cmd::Register<CommandId::SetMaterialTexture>(im,
		[](EngineContext* ctx, const SetMaterialTextureCmd& c)
		{
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c.material)) {
				const TextureId tid = ctx->GetTextureManager()->InternTexture(c.texture);
				std::vector<TextureId>& variants = m->textures[static_cast<TextureSlotRole>(c.role)];
				// Вне диапазона (список успели укоротить между кадром UI и исполнением) — тихо мимо:
				// это гонка, а не ошибка.
				if (variants.empty()) variants.push_back(tid);
				else if (c.variant < variants.size()) variants[c.variant] = tid;
				ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c.material);
			}
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
		});

	cmd::Register<CommandId::CreateMaterial>(im,
		[](EngineContext* ctx, const CreateMaterialCmd& c)
		{
			ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram("Lit");
			std::vector<std::pair<TextureSlotRole, std::vector<TextureId>>> texs;
			if (sp) for (TextureSlotRole role : sp->required_slots)
				texs.emplace_back(role, std::vector<TextureId>{ ctx->GetTextureManager()->InternTexture(DefaultTextureForRole(role)) });
			Material* m = ctx->GetMaterialManager()->CreateMaterial(c.name, std::move(texs), std::vector<ShaderProgramId>{ ctx->GetShaderManager()->InternShaderProgram("Lit") });
			if (m) ctx->SetMaterialParams(m, "Lit", OpaqueMaterialParams{});   // блоб адресован Lit: её MaterialBlock
			ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c.name);
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
		});

	// Дефолтами добираем ТОЛЬКО новые роли: общую с другой sp текстуру роли не перетираем.
	cmd::Register<CommandId::AddMaterialShader>(im,
		[](EngineContext* ctx, const MaterialShaderCmd& c)
		{
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c.material)) {
				const ShaderProgramId sp_id = ctx->GetShaderManager()->InternShaderProgram(c.shader);
				bool present = false;
				for (auto& b : m->shader_programs) if (b.sp == sp_id) { present = true; break; }
				if (!present) {
					// params остаются пустыми: раскладку cbuffer движок не выводит, тип выбирают в инспекторе.
					m->shader_programs.push_back(SpBinding{ sp_id, nullptr, {} });
					if (ShaderProgram* sp = ctx->GetShaderManager()->GetShaderProgram(c.shader))
						for (TextureSlotRole role : sp->required_slots)
							if (!m->textures.count(role)) m->textures[role] = { ctx->GetTextureManager()->InternTexture(DefaultTextureForRole(role)) };
					ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c.material);
					ctx->GetBatchBuilder()->SetDirtyBatches(true);
				}
			}
		});

	// Leftover-роли в textures не чистим — безвредны, просто не используются.
	cmd::Register<CommandId::RemoveMaterialShader>(im,
		[](EngineContext* ctx, const MaterialShaderCmd& c)
		{
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c.material)) {
				auto& sps = m->shader_programs;
				const ShaderProgramId sp_id = ctx->GetShaderManager()->ShaderProgramIdOf(c.shader);
				for (size_t i = 0; i < sps.size(); ++i) if (sps[i].sp == sp_id) {
					sps.erase(sps.begin() + i);
					break;
				}
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	// Новый вариант — КОПИЯ дефолта, чтобы резолвился сразу, а не давал dummy. Потолок
	// MAX_UVL_BLOCKS не здесь: он на пару (материал, sp), считает и логирует его BatchBuilder.
	cmd::Register<CommandId::AddMaterialTextureVariant>(im,
		[](EngineContext* ctx, const MaterialVariantCmd& c)
		{
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c.material)) {
				std::vector<TextureId>& variants = m->textures[static_cast<TextureSlotRole>(c.role)];
				variants.push_back(variants.empty() ? TextureId{} : variants[0]);
				ctx->GetMaterialManager()->CollectSamplerUsage(m, ctx->GetTextureManager(), c.material);
				// Структурная правка: сменились длина таблицы UVL и нумерация ячеек секции.
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	// Ноль — ДЕФОЛТ слота, его не убрать («убрать текстуру у слота» — другая операция, её тут нет).
	// Протухшие номера на сущностях не подрезаем: кламп v >= count в шейдере вернёт дефолт.
	cmd::Register<CommandId::RemoveMaterialTextureVariant>(im,
		[](EngineContext* ctx, const MaterialVariantCmd& c)
		{
			if (Material* m = ctx->GetMaterialManager()->GetMaterial(c.material)) {
				auto it = m->textures.find(static_cast<TextureSlotRole>(c.role));
				if (it != m->textures.end() && c.variant > 0 && c.variant < it->second.size()) {
					it->second.erase(it->second.begin() + c.variant);
					ctx->GetBatchBuilder()->SetDirtyBatches(true);
				}
			}
		});

	// НЕ структурная правка — в этом вся идея фичи: два куба с одним материалом показывают разное,
	// оставаясь в одном инстанс-батче.
	cmd::Register<CommandId::SetEntityTextureVariant>(im,
		[](EngineContext* ctx, const EntityTextureVariantCmd& c)
		{
			ctx->SetEntityTextureVariant(c.entity, c.mat_index,
				static_cast<TextureSlotRole>(c.role), c.variant);
		});

	// Материалы резолвятся по имени — ре-кей требует пересборки.
	cmd::Register<CommandId::RenameMaterial>(im,
		[](EngineContext* ctx, const RenameMaterialCmd& c)
		{
			MaterialManager* mtm = ctx->GetMaterialManager();
			if (mtm->RenameMaterial(mtm->MaterialIdOf(c.oldName), c.newName))
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
		});
}

void DefaultCommandSet::SetTextureCommands(InputManager& im)
{
	// Пустой old_name — создание, иначе правка выбранной ячейки. Ребилд батчей: материалы держат
	// id ячейки и подхватят новый хэндл сами.
	cmd::Register<CommandId::UpsertTexture>(im,
		[](EngineContext* ctx, const UpsertTextureCmd& c)
		{
			if (!c.name.empty() && !c.atlas.empty() && !c.path.empty()) {
				TextureManager* tm = ctx->GetTextureManager();
				const TextureId edited = tm->TextureIdOf(c.old_name);
				if (const TextureId taken = tm->TextureIdOf(c.name); taken && taken != edited) {
					SDL_Log("UpsertTexture: '%s' is taken by another texture - refused", c.name.c_str());
					return;
				}
				tm->RenameTexture(edited, c.name);
				const TextureId tex_id = tm->InternTexture(c.name);
				const TextureHandle* prev = tm->GetTextureHandle(tex_id);
				const ResourceTag keep = prev ? prev->tags : ResourceTag::None;
				tm->DeleteTextureHandle(tex_id, NameSlot::Keep);   // replace в той же ячейке (no-op, если пуста)
				// ReleasePreview НЕ зовём: ячейка та же, слот превью должен пережить пересоздание
				// (иначе плитка мигнёт затычкой до нового блита).
				// Куб — ОДИН хэндл на 6 слоёв, поэтому снятие, превью и ренейм выше для него те же строки.
				if (c.cube) ctx->CreateCubeMapTexture(c.name, c.atlas, c.path.c_str(), keep);
				else         ctx->CreateTextureFromFile(c.name, c.atlas, c.path.c_str(), static_cast<ChannelConvention>(c.conv), keep);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	cmd::Register<CommandId::DeleteTexture>(im,
		[](EngineContext* ctx, const DeleteTextureCmd& c)
		{
			TextureManager* tm = ctx->GetTextureManager();
			const TextureId id = tm->TextureIdOf(c.name);
			tm->DeleteTextureHandle(id, NameSlot::Release);
			tm->ReleasePreview(id);   // реальное удаление → освободить превью-ячейку
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
		});
}

void DefaultCommandSet::SetModelCommands(InputManager& im)
{
	// Перезагрузка in-place: ModelData остаётся в той же ячейке, ModelId у сущностей жив — чинить
	// ссылки не нужно. Пересборка — из-за диапазонов геометрии: их батч запекает у себя.
	cmd::Register<CommandId::UpsertModel>(im,
		[](EngineContext* ctx, const UpsertModelCmd& c)
		{
			if (!c.name.empty() && !c.model_path.empty() && !c.index_path.empty()) {
				ModelManager* mm = ctx->GetModelManager();
				const ModelId edited = mm->ModelIdOf(c.old_name);
				if (const ModelId taken = mm->ModelIdOf(c.name); taken && taken != edited) {
					SDL_Log("UpsertModel: '%s' is taken by another model - refused", c.name.c_str());
					return;
				}
				mm->RenameModel(edited, c.name);
				mm->LoadModelFromFile(c.name, c.model_path, c.index_path,
					static_cast<AnchorShift>(c.anchor));
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});
}

void DefaultCommandSet::SetShaderCommands(InputManager& im)
{
	// Материалы с удалённой sp уходят на fallback — это делает BatchBuilder на пересборке.
	cmd::Register<CommandId::DeleteShader>(im,
		[](EngineContext* ctx, const ShaderProgramNameCmd& c)
		{
			ShaderManager* smgr = ctx->GetShaderManager();
			if (const ShaderProgramId id = smgr->ShaderProgramIdOf(c.shader); smgr->GetShaderProgram(id)) {
				smgr->DeleteShaderProgram(id, NameSlot::Release);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	// Одна форма на правку и создание: старая sp найдена (oldName) → delete+create, пустой oldName
	// (плитка «+») → создание с нуля.
	cmd::Register<CommandId::RecreateShader>(im,
		[](EngineContext* ctx, const RecreateShaderCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			const ShaderProgramId old_id = sm->ShaderProgramIdOf(c.oldName);
			ShaderProgram* old = sm->GetShaderProgram(old_id);   // nullptr = создание с нуля

			// При ренейме ссылки материалов по СТАРОМУ имени НЕ чиним — конвенция движка: на пересборке
			// они дадут fallback.
			std::string finalName;
			if (old)
				finalName = (!c.newName.empty() && c.newName != c.oldName
					&& !sm->ShaderProgramIdOf(c.newName)) ? c.newName : c.oldName;
			else {
				if (c.newName.empty() || sm->ShaderProgramIdOf(c.newName)) { return; }
				finalName = c.newName;
			}

			std::string passName = c.passName;
			if (!ctx->GetPassManager()->GetRenderPassStep(passName) && old) passName = old->render_pass_name;
			const std::vector<BufferDataName>   vbufs = c.vsBuffers;
			const std::vector<BufferDataName>   fbufs = c.fsBuffers;
			const std::vector<TextureSlotRole>  slots = c.slots;   // дубли отсеет CreateShaderProgram
			const std::string vsName = !c.vsName.empty() ? c.vsName : (old ? sm->VertexShaders().NameOf(old->vs_id) : std::string());
			const std::string fsName = !c.fsName.empty() ? c.fsName : (old ? sm->FragmentShaders().NameOf(old->fs_id) : std::string());

			const ResourceTag keep = old ? old->tags : ResourceTag::None;
			if (old) {
				if (finalName != c.oldName) sm->RenameShaderProgram(old_id, finalName);
				sm->DeleteShaderProgram(old_id, NameSlot::Keep);
			}
			// push-инструкции не переносим руками: CreateShaderProgram сам возьмёт код-байндинги из
			// реестра ПО ИМЕНИ. Переименование = смена владельца функции — перенос со старого
			// имени всё равно жил бы лишь до ближайшей LoadScene, где связывает имя.
			sm->CreateShaderProgram(finalName, c.spd, passName, vsName, vbufs, fsName, fbufs, slots, ctx->GetBufferManager(), keep);
			sm->SetDirtyGraphicsPipelines(true);
			ctx->GetBatchBuilder()->SetDirtyBatches(true);
		});

	cmd::Register<CommandId::UpsertVertexShader>(im,
		[](EngineContext* ctx, const UpsertVertexShaderCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c.name.empty() && !c.path.empty()) {
				const VertexShaderId edited = sm->VertexShaders().Find(c.oldName);
				if (const VertexShaderId taken = sm->VertexShaders().Find(c.name); taken && taken != edited) {
					SDL_Log("UpsertVertexShader: '%s' is taken by another shader - refused", c.name.c_str());
					return;
				}
				sm->RenameVertexShader(edited, c.name);
				const VertexShaderData* prev = sm->GetVertexShader(sm->VertexShaders().Find(c.name));
				const ResourceTag keep = prev ? prev->tags : ResourceTag::None;
				sm->CreateVertexShader(c.name, c.path.c_str(), ctx->GetModelManager()->GetPool(c.pool),
					c.pull, ctx->GetBufferManager(), c.defines, keep);
				const VertexShaderId vs_id = sm->VertexShaders().Find(c.name);
				for (int32_t i = 0; i < sm->ShaderPrograms().Count(); ++i)
					if (ShaderProgram* spp = sm->ShaderPrograms().At(i).object.get(); spp && spp->vs_id == vs_id)
						spp->pipeline.reset();
				sm->SetDirtyGraphicsPipelines(true);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	cmd::Register<CommandId::UpsertFragmentShader>(im,
		[](EngineContext* ctx, const UpsertFragmentShaderCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c.name.empty() && !c.path.empty()) {
				const FragmentShaderId edited = sm->FragmentShaders().Find(c.oldName);
				if (const FragmentShaderId taken = sm->FragmentShaders().Find(c.name); taken && taken != edited) {
					SDL_Log("UpsertFragmentShader: '%s' is taken by another shader - refused", c.name.c_str());
					return;
				}
				sm->RenameFragmentShader(edited, c.name);
				const FragmentShaderData* prev = sm->GetFragmentShader(sm->FragmentShaders().Find(c.name));
				const ResourceTag keep = prev ? prev->tags : ResourceTag::None;
				sm->CreateFragmentShader(c.name, c.path.c_str(), c.defines, keep);
				const FragmentShaderId fs_id = sm->FragmentShaders().Find(c.name);
				for (int32_t i = 0; i < sm->ShaderPrograms().Count(); ++i)
					if (ShaderProgram* spp = sm->ShaderPrograms().At(i).object.get(); spp && spp->fs_id == fs_id)
						spp->pipeline.reset();
				sm->SetDirtyGraphicsPipelines(true);
				ctx->GetBatchBuilder()->SetDirtyBatches(true);
			}
		});

	cmd::Register<CommandId::UpsertComputeShader>(im,
		[](EngineContext* ctx, const UpsertComputeShaderCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			if (!c.name.empty() && !c.path.empty()) {
				const ComputeShaderId edited = sm->ComputeShaders().Find(c.oldName);
				if (const ComputeShaderId taken = sm->ComputeShaders().Find(c.name); taken && taken != edited) {
					SDL_Log("UpsertComputeShader: '%s' is taken by another shader - refused", c.name.c_str());
					return;
				}
				sm->RenameComputeShader(edited, c.name);
				const ComputeShaderData* prev = sm->GetComputeShader(sm->ComputeShaders().Find(c.name));
				const ResourceTag keep = prev ? prev->tags : ResourceTag::None;
				sm->CreateComputeShader(c.name, c.path.c_str(), c.defines, keep);
				const ComputeShaderId cs_id = sm->ComputeShaders().Find(c.name);
				for (int32_t i = 0; i < sm->ComputePrograms().Count(); ++i)
					if (ComputeShaderProgram* csp = sm->ComputePrograms().At(i).object.get(); csp && csp->cs_id == cs_id)
						csp->pipeline.reset();
				sm->SetDirtyComputePipelines(true);
				sm->SetDirtyComputeBatches(true);
			}
		});

	cmd::Register<CommandId::DeleteVertexShader>(im,
		[](EngineContext* ctx, const ShaderDataNameCmd& c)
		{
			// Используемый SD менеджер удалить откажется (пайплайн собран из его данных, fallback
			// с чужой раскладкой невозможен); неиспользуемый ничего не рисует — dirty-флаги не нужны.
			ShaderManager* sm = ctx->GetShaderManager();
			sm->DeleteVertexShader(sm->VertexShaders().Find(c.name), NameSlot::Release);
		});

	cmd::Register<CommandId::DeleteFragmentShader>(im,
		[](EngineContext* ctx, const ShaderDataNameCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			sm->DeleteFragmentShader(sm->FragmentShaders().Find(c.name), NameSlot::Release);   // отказ/чистое удаление — см. DeleteVertexShader
		});

	cmd::Register<CommandId::DeleteComputeShader>(im,
		[](EngineContext* ctx, const ShaderDataNameCmd& c)
		{
			ShaderManager* sm = ctx->GetShaderManager();
			sm->DeleteComputeShader(sm->ComputeShaders().Find(c.name), NameSlot::Release);   // отказ/чистое удаление — см. DeleteVertexShader
		});
}
