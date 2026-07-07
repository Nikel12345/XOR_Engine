#include "PCH.h"
#include "DefaultUpdateSet.h"
#include "EngineContext.h"
#include "LightDataModule.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "InstanceDataModule.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"

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
    },
        [mm]() -> uint32_t
    {
        return mm->GetVertexBaseOffset();
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
    },
        [mm]() -> uint32_t
    {
        return mm->GetIndexBaseOffset();
    }
    );
    index_update_inited = true;
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

void DefaultUpdateSet::SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm)
{
    if (indirect_update_inited) {
        SDL_Log("Default indirect updater is already initialized.");
        return;
    }
    auto* bm = ctx.GetBufferManager();
    auto* pm = ctx.GetPassManager();
    auto* om = ctx.GetObjectManager();

    // PER-FRAME (без revision-гейта): num_instances у каждой копии обязан быть 0 в начале
    // кадра (scatter накапливает атомиком). num_cameras = 1 игрок + L световых.
    bm->CreateUpdateInstruction(DEFAULT_INDIRECT_BUFFER,
        [pm, idm, om, ldm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        uint32_t nc = 1 + ldm->AskNumLightCameras(om, om->GetActiveScene());
        idm->StoreIndirect(bm, pm, &task, nc);
    },
        [pm, idm, om, ldm]() -> uint32_t
    {
        uint32_t nc = 1 + ldm->AskNumLightCameras(om, om->GetActiveScene());
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

    // Сферы по строкам трансформов для GPU-каллинга. Тот же gate-паттерн, что у PIB:
    // ревизия батчей per-slot (создание/удаление энтити двигает строки).
    bm->CreateUpdateInstruction(DEFAULT_BOUND_SPHERE_BUFFER,
        [om, bdm](SDL_GPUCopyPass* cp, BufferManager* bm, UploadTask& task)
    {
        bdm->StoreSpheres(bm, &task, om);
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
    auto* om = ctx.GetObjectManager();
    auto* bb = ctx.GetBatchBuilder();

    // Только ресайз (updater = nullptr): содержимое пишет scatter-каллинг каждый кадр
    // (компактно). Блок int'ов на камеру (N = число PIB-записей): блок 0 — камера игрока.
    bm->CreateUpdateInstruction(DEFAULT_OUT_PIB_BUFFER,
        nullptr,
        [om, bb, ldm]() -> uint32_t
    {
        uint32_t num_cameras_total = ldm->AskNumLightCameras(om, om->GetActiveScene()) + 1;
        return num_cameras_total * bb->AskNumInstances() * sizeof(int32_t);
    }
    );
    out_pib_update_inited = true;
}