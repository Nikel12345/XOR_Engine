#include "PCH.h"
#include "DefaultUpdateSet.h"
#include "EngineContext.h"
// EngineContext.h держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "BufferManager.h"
#include "CameraManager.h"
#include "ModelManager.h"
#include "RenderManager.h"
#include "LightDataModule.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "InstanceDataModule.h"
#include "TextureStateDataModule.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"
#include "UI_DataModule.h"
#include "FontManager.h"
#include "ObjectManager.h"
#include "BatchBuilder.h"

namespace DefaultUpdateSet
{
    bool camera_update_inited = false;
    bool position_update_inited = false;
    bool light_update_inited = false;
    bool position_index_update_inited = false;
    bool light_cameras_update_inited = false;
    bool indirect_update_inited = false;
    bool bound_sphere_update_intited = false;
    bool out_pib_update_inited = false;
}

using namespace DefaultBuffersNames;

void DefaultUpdateSet::SetDefaultCameraUpdater(EngineContext& ctx)
{
    if (camera_update_inited) {
        SDL_Log("Default camera updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* cm = ctx.GetCameraManager();
    bm->CreateUpdateInstruction(DEFAULT_CAMERA_BUFFER,
        [cm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        cm->StoreActiveCamera(bm, &task);
    },
        [cm]() -> uint32_t
    {
        return cm->CalculateCameraSize();
    }
    );
    camera_update_inited = true;
}

void DefaultUpdateSet::SetDefaultPositionUpdater(EngineContext& ctx, TransformDataModule* tdm)
{
    if (position_update_inited) {
        SDL_Log("Default position updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    bm->CreateUpdateInstruction(DEFAULT_TRANSFORM_BUFFER,
        [om, tdm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        tdm->StoreTransforms(bm, &task, om, scene);
    },
        [om, tdm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return 0;
        return tdm->CalculateTransformSize(om, scene);
    }
    );
    position_update_inited = true;
}

void DefaultUpdateSet::SetDefaultInstanceDataUpdater(EngineContext& ctx, InstanceDataModule* idm)
{
    static bool instance_update_inited = false;
    if (instance_update_inited) {
        SDL_Log("Default instance data updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    // Каждый кадр (без dirty): тот же порядок строк, что у трансформов (см. InstanceDataModule).
    bm->CreateUpdateInstruction(DEFAULT_INSTANCE_BUFFER,
        [om, idm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        idm->StoreInstanceData(bm, &task, om, scene);
    },
        [om, idm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return 0;
        return idm->CalculateInstanceSize(om, scene);
    }
    );
    instance_update_inited = true;
}

void DefaultUpdateSet::SetDefaultLightUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    if (light_update_inited) {
        SDL_Log("Default light updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    bm->CreateUpdateInstruction(DEFAULT_LIGHT_BUFFER,
        [om, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        ldm->StoreLightData(bm, &task, om, scene);
    },
        [om, ldm, bm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return 0;
        // Ёмкость буфера слота — она же счётчик источников у шейдера (GetDimensions), см.
        // LightDataModule.h: пишем буфер целиком, иначе смена сцены оставляет в нём чужой свет.
        BufferData* bd = bm->GetBufferData(DEFAULT_LIGHT_BUFFER);
        const uint32_t capacity = bd ? bd->Dynamic.buffer_size[bm->logic_index.load()] : 0;
        return ldm->CalculateLightSize(om, scene, capacity);
    }
    );
    light_update_inited = true;
}

void DefaultUpdateSet::SetDefaultPositionIndexUpdater(EngineContext& ctx, PIB_DataModule* pib_dm)
{
    if (position_index_update_inited) {
        SDL_Log("Default position index updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* rm = ctx.GetPassManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();

    // Dirty-состояние держит сам модуль; сюда лишь читаем ревизию батчей и отдаём
    // числом — так модуль не тянет BatchBuilder.h. Если ревизия не менялась,
    // CalculatePIBSizes вернёт 0 и store пропустится.
    bm->CreateUpdateInstruction(DEFAULT_POSITION_INDEX_BUFFER,
        [rm, om, pib_dm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        pib_dm->StorePIB(bm, rm, &task, om);
    },
        [rm, pib_dm, bb, bm]() -> uint32_t
    {
        return pib_dm->CalculatePIBSizes(rm, bb->BatchesRevision(), bm->logic_index.load());
    }
    );
    position_index_update_inited = true;
}

void DefaultUpdateSet::SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    if (light_cameras_update_inited) {
        SDL_Log("Default light cameras updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    // Порядок регистрации больше НЕ значим: слепок камер пишет фаза слепков в PrepareFunc,
    // а size-функции только читают его.
    bm->CreateUpdateInstruction(DEFAULT_LIGHT_CAMERA_BUFFER,
        [om, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        ldm->StoreLightCameras(bm, &task, om, scene);
    },
        [ldm, bm]() -> uint32_t
    {
        // Читалка: слепок камер к этому моменту уже записан фазой слепков в PrepareFunc.
        return ldm->CalculateLightCamerasSize(bm->logic_index.load());
    }
    );
    light_cameras_update_inited = true;
}

void DefaultUpdateSet::SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm)
{
    if (indirect_update_inited) {
        SDL_Log("Default indirect updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();
    auto* bb = ctx.GetBatchBuilder();

    // Гейт по ПАРЕ {ревизия батчей, штамп регионов слота}: от них зависит содержимое команд,
    // а покадрово меняется единственное поле num_instances — его обнуляет culling_clear на GPU
    // перед каждым scatter (WARNINGS.md). Оба входа — от фазы слепков этого же слота.
    // Гейт безопасен при ресайзе (буфер RESIZE_ONLY, содержимое теряется): размер — функция
    // от той же раскладки, значит ресайз невозможен без смены ключа.
    bm->CreateUpdateInstruction(DEFAULT_INDIRECT_BUFFER,
        [pm, idm, ldm, bb](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        idm->StoreIndirect(bm, pm, &task, pm->AskRegions(bm->logic_index.load()));
    },
        [pm, idm, bb, bm]() -> uint32_t
    {
        const uint8_t slot = bm->logic_index.load();
        return idm->CalculateIndirectSize(pm->AskRegions(slot), bb->BatchesRevision(), slot);
    }
    );
    indirect_update_inited = true;
}

void DefaultUpdateSet::SetDefaultEntityToCmdUpdater(EngineContext& ctx, PIB_DataModule* pib_dm)
{
    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();
    auto* bb = ctx.GetBatchBuilder();

    // Гейт по ревизии батчей (меняется только со структурой), как у PIB.
    bm->CreateUpdateInstruction(DEFAULT_ENTITY_TO_CMD_BUFFER,
        [pm, pib_dm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        pib_dm->StoreEntityToCmd(bm, pm, &task);
    },
        [pm, pib_dm, bb, bm]() -> uint32_t
    {
        return pib_dm->CalculateEntityToCmd(pm, bb->BatchesRevision(), bm->logic_index.load());
    }
    );
}

void DefaultUpdateSet::SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm)
{
    if (bound_sphere_update_intited) {
        SDL_Log("Default bound sphere updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();
    auto* mm = ctx.GetModelManager();   // резолвер имени модели у энтити (см. StoreSpheres)

    // Сферы по строкам трансформов для GPU-каллинга. Тот же gate-паттерн, что у PIB:
    // ревизия батчей per-slot (создание/удаление энтити двигает строки).
    bm->CreateUpdateInstruction(DEFAULT_BOUND_SPHERE_BUFFER,
        [om, bdm, mm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        bdm->StoreSpheres(bm, &task, om, mm);
    },
        [om, bdm, bb, bm]() -> uint32_t
    {
        return bdm->CalculateSphereSize(om, bb->BatchesRevision(), bm->logic_index.load());
    }
    );
    bound_sphere_update_intited = true;
}

void DefaultUpdateSet::SetDefaultOutPibUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    if (out_pib_update_inited) {
        SDL_Log("Default out pib updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* bb = ctx.GetBatchBuilder();
    auto* pm = ctx.GetPassManager();

    // Только ресайз (updater = nullptr): содержимое пишет scatter-каллинг каждый кадр
    // (компактно). Регион на ПРОХОД, внутри — блок int'ов на каждый его дроу за кадр, и в
    // блоке только его записи: сумма blocks*pib, а не (1+L)*N. Раскладку берём из слепков слота
    // (камеры пишет size-фаза LIGHT_CAMERA_BUFFER выше, раскладку батчей —
    // StampLayoutSnapshot в PrepareFunc до апдейтеров).
    bm->CreateUpdateInstruction(DEFAULT_OUT_PIB_BUFFER,
        nullptr,
        [pm, bm]() -> uint32_t
    {
        return pm->AskRegions(bm->logic_index.load()).total_pib * sizeof(int32_t);
    }
    );
    out_pib_update_inited = true;
}

void DefaultUpdateSet::SetDefaultTexStateUpdaters(EngineContext& ctx, TextureStateDataModule* tsm)
{
    static bool tex_state_update_inited = false;
    if (tex_state_update_inited) {
        SDL_Log("Default texture state updaters are already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* mtm = ctx.GetMaterialManager();   // резолвер имени материала, ПАРАМЕТРОМ (см. CLAUDE.md)
    auto* bb = ctx.GetBatchBuilder();

    // ПОРЯДОК РЕГИСТРАЦИИ ЗНАЧИМ: size_fn'ы гоняются в нём, а канал строит первый из них
    // (CalculateRankSize) — index берёт готовое число носителей. Тот же приём, что у UI_DataModule
    // с его BuildStaging в size-фазе первого буфера.
    //
    // rank и index — послотный гейт по ревизии батчей: они зависят от порядка строк, наличия тега
    // и числа материалов, то есть только от структуры. Состояния ниже гейта не имеют — их домен
    // фильтруется тегом, и смену номера варианта ревизия батчей не видит (см. модуль).
    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_RANK_BUFFER,
        [tsm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        tsm->StoreRank(bm, &task);
    },
        [om, tsm, bb, bm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        return scene ? tsm->CalculateRankSize(om, scene, bb->BatchesRevision(),
                                              bm->logic_index.load()) : 0u;
    }
    );

    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_INDEX_BUFFER,
        [tsm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        tsm->StoreIndex(bm, &task);
    },
        [tsm, bb, bm]() -> uint32_t
    {
        return tsm->CalculateIndexSize(bb->BatchesRevision(), bm->logic_index.load());
    }
    );

    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_BUFFER,
        [om, tsm, mtm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        if (SceneData* scene = om->GetActiveScene()) tsm->StoreState(bm, &task, om, scene, mtm);
    },
        [om, tsm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        return scene ? tsm->CalculateStateSize(om, scene) : 0u;
    }
    );
    tex_state_update_inited = true;
}

void DefaultUpdateSet::SetUITextUpdaters(EngineContext& ctx, UI_DataModule* uidm, FontManager* fm, const std::string& fontName)
{
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    // RANK — ПЕРВЫЙ: его size-фаза строит staging (BuildStaging по активной сцене), остальные два
    // текстовых буфера лишь читают уже готовый staging (тот же контракт, что LIGHT_CAMERA_BUFFER).
    bm->CreateUpdateInstruction(UI_TEXT_RANK_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreRank(bm, &task); },
        [uidm, om]() -> uint32_t { uidm->BuildStaging(om); return uidm->CalcRankSize(); });

    bm->CreateUpdateInstruction(UI_TEXT_INDEX_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreIndex(bm, &task); },
        [uidm]() -> uint32_t { return uidm->CalcIndexSize(); });

    bm->CreateUpdateInstruction(UI_TEXT_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreText(bm, &task); },
        [uidm]() -> uint32_t { return uidm->CalcTextSize(); });

    // GlyphUVL — из FontManager по имени шрифта (резолв на лету: до создания шрифта size 0, апдейт
    // пропускается). Валидно после PackAtlases — ExecuteUpdateInstructions идёт позже в PrepareFunc.
    bm->CreateUpdateInstruction(UI_FONT_UVL_BUFFER,
        [fm, fontName](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { fm->StoreGlyphUVL(fm->GetFont(fontName), bm, &task); },
        [fm, fontName]() -> uint32_t { return fm->GlyphUvlBytes(fm->GetFont(fontName)); });
}