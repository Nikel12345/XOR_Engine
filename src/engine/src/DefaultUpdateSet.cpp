#include "PCH.h"
#include "DefaultUpdateSet.h"
#include "EngineContext.h"
#include "LightDataModule.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"
#include "CountBufferDataModule.h"

namespace DefaultUpdateSet
{
    bool camera_update_inited = false;
    bool position_update_inited = false;
    bool light_update_inited = false;
    bool position_index_update_inited = false;
    bool vertex_update_inited = false;
    bool index_update_inited = false;
    bool light_cameras_update_inited = false;
    bool indirect_update_inited = false;
    bool bound_sphere_update_intited = false;
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
    auto* rm = ctx.GetRenderManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreateUpdateInstruction(DEFAULT_POSITION_INDEX_BUFFER,
        [rm, om, pib_dm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        pib_dm->StorePIB(bm, rm, &task, om);
    },
        [om, rm, pib_dm, bb]() -> uint32_t
    {
        return pib_dm->CalculatePIBSizes(bb, om, rm);
    }
    );
    position_index_update_inited = true;
}

void DefaultUpdateSet::SetDefaultVertexUpdater(EngineContext& ctx)
{
    if (vertex_update_inited) {
        SDL_Log("Default vertex updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* mm = ctx.GetModelManager();
    bm->CreateUpdateInstruction(DEFAULT_VERTEX_BUFFER,
        [mm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        mm->UploadModelVertexBuffer(bm, &task);
    },
        [mm]() -> uint32_t
    {
        return mm->CalculateModelsVerticesSize();
    }
    );
    vertex_update_inited = true;
}

void DefaultUpdateSet::SetDefaultIndexUpdater(EngineContext& ctx)
{
    if (index_update_inited) {
        SDL_Log("Default index updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* mm = ctx.GetModelManager();
    bm->CreateUpdateInstruction(DEFAULT_INDEX_BUFFER,
        [mm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        mm->UploadModelIndexBuffer(bm, &task);
    },
        [mm]() -> uint32_t
    {
        return mm->CalculateModelsIndicesSize();
    }
    );
}

void DefaultUpdateSet::SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm)
{
    if (light_cameras_update_inited) {
        SDL_Log("Default light cameras updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    bm->CreateUpdateInstruction(DEFAULT_LIGHT_CAMERA_BUFFER,
        [om, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return;
        ldm->StoreLightCameras(bm, &task, om, scene);
    },
        [om, ldm]() -> uint32_t
    {
        SceneData* scene = om->GetActiveScene();
        if (!scene) return 0;
        return ldm->CalculateLightCamerasSize(om, scene);
    }
    );
    light_cameras_update_inited = true;
}

void DefaultUpdateSet::SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm)
{
    if (indirect_update_inited) {
        SDL_Log("Default indirect updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetRenderManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreateUpdateInstruction(DEFAULT_INDIRECT_BUFFER,
        [pm, idm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        idm->StoreIndirect(bm, pm, &task);
    },
        [bb, pm, idm]() -> uint32_t
    {
        return idm->CalculateIndirectSize(bb, pm);
    }
    );
    indirect_update_inited = true;
}

void DefaultUpdateSet::SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm)
{
    if (bound_sphere_update_intited) {
        SDL_Log("Default bound sphere updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetRenderManager();
    auto* mm = ctx.GetModelManager();
    bm->CreatePrePassUpdateInstruction(DEFAULT_BOUND_SPHERE_BUFFER,
        [pm, bdm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        bdm->StoreSpheres(bm, &task, pm);
    },
        [pm, mm, bdm]() -> uint32_t
    {
        return bdm->CalculateSphereSize(pm, mm);
    }
    );
    bound_sphere_update_intited = true;
}

void DefaultUpdateSet::SetDefaultCountBufferUpdater(EngineContext& ctx, CountBufferDataModule* cdm, LightDataModule* ldm)
{
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreatePrePassUpdateInstruction(DEFAULT_COUNT_BUFFER,
        nullptr,
        [om, cdm, ldm, bb]() -> uint32_t
    {
        return cdm->CountBufferSize(om, om->GetActiveScene(), bb, ldm);
    }
    );
}

void DefaultUpdateSet::SetDefaultOffsetBufferUpdater(EngineContext& ctx, CountBufferDataModule* cdm, LightDataModule* ldm)
{
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreatePrePassUpdateInstruction(DEFAULT_OFFSET_BUFFER,
        nullptr,
        [om, cdm, ldm, bb]() -> uint32_t
    {
        return cdm->CountBufferSize(om, om->GetActiveScene(), bb, ldm) - 1;
    }
    );
}

void DefaultUpdateSet::SetDefaultEntityToBatchUpdater(EngineContext& ctx, PIB_DataModule* pdm)
{
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* pm = ctx.GetRenderManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreatePrePassUpdateInstruction(DEFAULT_ENTITY_TO_BATCH_BUFFER,
        [pdm, pm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        pdm->StoreEntityToBatch(bm, pm, &task);
    },
        [om, pm, bb, pdm]() -> uint32_t
    {
        return pdm->CalculateEntityToBatch(bb, om, pm);
    }
    );
}

void DefaultUpdateSet::SetDefaultOutTransformUpdater(EngineContext& ctx, TransformDataModule* tdm)
{
    auto* bm = ctx.GetBufferManager();
    bm->CreatePostReadbackUpdateInstruction(DEFAULT_OUT_TRANSFORM_BUFFER,
        nullptr,
        [tdm]() -> uint32_t
    {
        return tdm->CalculateOutTransformSize();
    }
    );
}

void DefaultUpdateSet::SetDefaultOutIndirectUpldater(EngineContext& ctx, LightDataModule* ldm)
{
    auto* bm = ctx.GetBufferManager();
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();
    bm->CreatePrePassUpdateInstruction(DEFAULT_OUT_INDIRECT_BUFFER,
        nullptr,
        [om, bb, ldm]() -> uint32_t
    {
        uint32_t num_cameras_total = ldm->AskNumLightCameras(om, om->GetActiveScene()) + 1;
        return num_cameras_total * bb->AskNumCommands() * sizeof(SDL_GPUIndexedIndirectDrawCommand);
    }
    );
}

void DefaultUpdateSet::SetDefaultCountReader(EngineContext& ctx, TransformDataModule* tdm)
{
    auto* bm = ctx.GetBufferManager();
    bm->CreateReadBackInstruction(DEFAULT_COUNT_BUFFER,
        [tdm](BufferManager* bm, ReadBackTask& task)
    {
        tdm->ReadBackCullingCountReader(bm, &task);
    },
        [tdm]() -> uint32_t
    {
        return tdm->ReadBackCullingCountSize();
    }
    );
}