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
        [om, ldm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return 0;
        return ldm->CalculateLightSize(om, scene);
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
    // ИНВАРИАНТ ПОРЯДКА: эта инструкция обязана регистрироваться РАНЬШЕ indirect/out_pib —
    // её size-фаза пишет слепок теневых камер слота (CalculateLightCamerasSize), который
    // они читают через AskNumLightCameras(slot) в своих size-фазах того же прохода.
    bm->CreateUpdateInstruction(DEFAULT_LIGHT_CAMERA_BUFFER,
        [om, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        ldm->StoreLightCameras(bm, &task, om, scene);
    },
        [om, ldm, bm]() -> uint32_t
    {
        // Size-фаза выполняется всегда (store при size==0 скипается) → слепок слота
        // никогда не остаётся стейлым, даже если последний теневой свет удалили.
        return ldm->CalculateLightCamerasSize(om, om->GetActiveScene(), bm->logic_index.load());
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

    // PER-FRAME (без revision-гейта): num_instances у каждой копии обязан быть 0 в начале
    // кадра (scatter накапливает атомиком). num_cameras = 1 игрок + L световых — из слепка
    // слота (его уже записала size-фаза LIGHT_CAMERA_BUFFER, см. инвариант порядка там).
    bm->CreateUpdateInstruction(DEFAULT_INDIRECT_BUFFER,
        [pm, idm, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        uint32_t nc = 1 + ldm->AskNumLightCameras(bm->logic_index.load());
        idm->StoreIndirect(bm, pm, &task, nc);
    },
        [pm, idm, ldm, bm]() -> uint32_t
    {
        uint32_t nc = 1 + ldm->AskNumLightCameras(bm->logic_index.load());
        return idm->CalculateIndirectSize(pm, nc);
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

    // Только ресайз (updater = nullptr): содержимое пишет scatter-каллинг каждый кадр
    // (компактно). Блок int'ов на камеру (N = число PIB-записей): блок 0 — камера игрока.
    // Оба множителя — из слепков слота (камеры пишет size-фаза LIGHT_CAMERA_BUFFER выше,
    // раскладку батчей — StampLayoutSnapshot в PrepareFunc до апдейтеров).
    bm->CreateUpdateInstruction(DEFAULT_OUT_PIB_BUFFER,
        nullptr,
        [bb, ldm, bm]() -> uint32_t
    {
        uint8_t slot = bm->logic_index.load();
        uint32_t num_cameras_total = ldm->AskNumLightCameras(slot) + 1;
        return num_cameras_total * bb->AskNumInstances(slot) * sizeof(int32_t);
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

    // Каждый кадр, без dirty-гейта: ревизия батчей тут не годится — переключение варианта дерево
    // не трогает намеренно, и гейт по ней пропускал бы ровно нужные кадры (см. TextureStateDataModule).
    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_PREFIX_BUFFER,
        [om, tsm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        if (SceneData* scene = om->GetActiveScene()) tsm->StorePrefix(bm, &task, om, scene);
    },
        [om, tsm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        return scene ? tsm->CalculatePrefixSize(om, scene) : 0u;
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

    // BITS — ПЕРВЫЙ: его size-фаза строит staging (BuildStaging по активной сцене), остальные три
    // текстовых буфера лишь читают уже готовый staging (тот же контракт, что LIGHT_CAMERA_BUFFER).
    bm->CreateUpdateInstruction(UI_TEXT_BITS_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreBits(bm, &task); },
        [uidm, om]() -> uint32_t { uidm->BuildStaging(om); return uidm->CalcBitsSize(); });

    bm->CreateUpdateInstruction(UI_TEXT_WORDBASE_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreWordBase(bm, &task); },
        [uidm]() -> uint32_t { return uidm->CalcWordBaseSize(); });

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