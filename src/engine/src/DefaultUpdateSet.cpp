#include "PCH.h"
#include "DefaultUpdateSet.h"
#include "EngineContext.h"
#include "BufferManager.h"
#include "CameraManager.h"
#include "ModelManager.h"
#include "PassManager.h"
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

using namespace DefaultBuffersNames;

void DefaultUpdateSet::SetDefaultCameraUpdater(EngineContext& ctx)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultCameraUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* cm = ctx.GetCameraManager();

    bm->CreateUpdateInstruction(DEFAULT_CAMERA_BUFFER,
        [cm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { cm->StoreActiveCamera(bm, &task); },
        [cm]() -> uint32_t { return cm->CalculateCameraSize(); });
}

void DefaultUpdateSet::SetDefaultPositionUpdater(EngineContext& ctx, TransformDataModule* tdm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultPositionUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    bm->CreateUpdateInstruction(DEFAULT_TRANSFORM_BUFFER,
        [om, tdm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (SceneData* scene = om->GetActiveScene()) tdm->StoreTransforms(bm, &task, om, scene); },
        [om, tdm]() -> uint32_t { SceneData* scene = om->GetActiveScene(); return scene ? tdm->CalculateTransformSize(om, scene) : 0u; });
}

void DefaultUpdateSet::SetDefaultInstanceDataUpdater(EngineContext& ctx, InstanceDataModule* idm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultInstanceDataUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    bm->CreateUpdateInstruction(DEFAULT_INSTANCE_BUFFER,
        [om, idm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (SceneData* scene = om->GetActiveScene()) idm->StoreInstanceData(bm, &task, om, scene); },
        [om, idm]() -> uint32_t { SceneData* scene = om->GetActiveScene(); return scene ? idm->CalculateInstanceSize(om, scene) : 0u; });
}

void DefaultUpdateSet::SetDefaultLightUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultLightUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    bm->CreateUpdateInstruction(DEFAULT_LIGHT_BUFFER,
        [om, ldm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (SceneData* scene = om->GetActiveScene()) ldm->StoreLightData(bm, &task, om, scene); },
        [om, ldm]() -> uint32_t { SceneData* scene = om->GetActiveScene(); return scene ? ldm->CalculateLightSize(om, scene) : 0u; });
}

void DefaultUpdateSet::SetDefaultPositionIndexUpdater(EngineContext& ctx, PIB_DataModule* pib_dm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultPositionIndexUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* pm = ctx.GetPassManager();
    auto* bb = ctx.GetBatchBuilder();

    bm->CreateUpdateInstruction(DEFAULT_POSITION_INDEX_BUFFER,
        [om, pm, pib_dm, bb](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { pib_dm->StorePIB(bm, pm, &task, om, bb->BatchesRevision(), bm->logic_index.load()); },
        [pm, pib_dm, bb, bm]() -> uint32_t { return pib_dm->CalculatePIBSizes(pm, bb->BatchesRevision(), bm->logic_index.load()); });
}

void DefaultUpdateSet::SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultLightCamerasUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    bm->CreateUpdateInstruction(DEFAULT_LIGHT_CAMERA_BUFFER,
        [om, ldm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (SceneData* scene = om->GetActiveScene()) ldm->StoreLightCameras(bm, &task, om, scene); },
        [ldm, bm]() -> uint32_t { return ldm->CalculateLightCamerasSize(bm->logic_index.load()); });
}

void DefaultUpdateSet::SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultIndirectUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();
    auto* bb = ctx.GetBatchBuilder();

    bm->CreateUpdateInstruction(DEFAULT_INDIRECT_BUFFER,
        [pm, idm, bb](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { const uint8_t slot = bm->logic_index.load(); idm->StoreIndirect(bm, pm, &task, pm->AskRegions(slot), bb->BatchesRevision(), slot); },
        [pm, idm, bb, bm]() -> uint32_t { const uint8_t slot = bm->logic_index.load(); return idm->CalculateIndirectSize(pm->AskRegions(slot), bb->BatchesRevision(), slot); });
}

void DefaultUpdateSet::SetDefaultEntityToCmdUpdater(EngineContext& ctx, PIB_DataModule* pib_dm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultEntityToCmdUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();
    auto* bb = ctx.GetBatchBuilder();

    bm->CreateUpdateInstruction(DEFAULT_ENTITY_TO_CMD_BUFFER,
        [pm, pib_dm, bb](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { pib_dm->StoreEntityToCmd(bm, pm, &task, bb->BatchesRevision(), bm->logic_index.load()); },
        [pm, pib_dm, bb, bm]() -> uint32_t { return pib_dm->CalculateEntityToCmd(pm, bb->BatchesRevision(), bm->logic_index.load()); });
}

void DefaultUpdateSet::SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultBoundSphereUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* mm = ctx.GetModelManager();

    bm->CreateUpdateInstruction(DEFAULT_BOUND_SPHERE_BUFFER,
        [om, mm, bdm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { bdm->StoreSpheres(bm, &task, om, mm, om->EntityRevision() + mm->SpheresRevision(), bm->logic_index.load()); },
        [om, mm, bdm, bm]() -> uint32_t { return bdm->CalculateSphereSize(om, om->EntityRevision() + mm->SpheresRevision(), bm->logic_index.load()); });
}

void DefaultUpdateSet::SetDefaultOutPibUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultOutPibUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();

    bm->CreateUpdateInstruction(DEFAULT_OUT_PIB_BUFFER,
        nullptr,
        [pm, bm]() -> uint32_t { return pm->AskRegions(bm->logic_index.load()).total_pib * sizeof(int32_t); });
}

void DefaultUpdateSet::SetDefaultTexStateChannel(EngineContext& ctx, TextureStateDataModule* tsm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultTexStateChannel: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();

    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_RANK_BUFFER,
        [om, tsm, bb](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (om->GetActiveScene()) tsm->StoreRank(bm, &task, bb->BatchesRevision(), bm->logic_index.load()); },
        [om, tsm, bb, bm]() -> uint32_t { SceneData* scene = om->GetActiveScene(); return scene ? tsm->CalculateRankSize(om, scene, bb->BatchesRevision(), bm->logic_index.load()) : 0u; });

    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_INDEX_BUFFER,
        [tsm, bb](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { tsm->StoreIndex(bm, &task, bb->BatchesRevision(), bm->logic_index.load()); },
        [tsm, bb, bm]() -> uint32_t { return tsm->CalculateIndexSize(bb->BatchesRevision(), bm->logic_index.load()); });
}

void DefaultUpdateSet::SetDefaultTexStateUpdater(EngineContext& ctx, TextureStateDataModule* tsm)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetDefaultTexStateUpdater: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* mtm = ctx.GetMaterialManager();

    bm->CreateUpdateInstruction(DEFAULT_TEX_STATE_BUFFER,
        [om, mtm, tsm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { if (SceneData* scene = om->GetActiveScene()) tsm->StoreState(bm, &task, om, scene, mtm); },
        [om, tsm]() -> uint32_t { SceneData* scene = om->GetActiveScene(); return scene ? tsm->CalculateStateSize(om, scene) : 0u; });
}

void DefaultUpdateSet::SetUITextUpdaters(EngineContext& ctx, UI_DataModule* uidm, FontManager* fm, const std::string& fontName)
{
    static bool inited = false;
    if (inited) { SDL_Log("DefaultUpdateSet::SetUITextUpdaters: already initialized"); return; }
    inited = true;

    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();

    // BuildStaging здесь считает размеры ВСЕХ трёх буферов текста разом (UI_DataModule.cpp), а
    // Calc* ниже — геттеры его полей. Поэтому вынести ранг отсюда нельзя: индекс и текст читают
    // то, что построила его size-фаза.
    bm->CreateUpdateInstruction(UI_TEXT_RANK_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreRank(bm, &task); },
        [uidm, om]() -> uint32_t { uidm->BuildStaging(om); return uidm->CalcRankSize(); });

    bm->CreateUpdateInstruction(UI_TEXT_INDEX_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreIndex(bm, &task); },
        [uidm]() -> uint32_t { return uidm->CalcIndexSize(); });

    bm->CreateUpdateInstruction(UI_TEXT_BUFFER,
        [uidm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { uidm->StoreText(bm, &task); },
        [uidm]() -> uint32_t { return uidm->CalcTextSize(); });

    bm->CreateUpdateInstruction(UI_FONT_UVL_BUFFER,
        [fm, fontName](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) { fm->StoreGlyphUVL(fm->GetFont(fontName), bm, &task); },
        [fm, fontName]() -> uint32_t { return fm->GlyphUvlBytes(fm->GetFont(fontName)); });
}
