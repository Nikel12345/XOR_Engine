#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "LightStruct.h"
#include "RenderSnapshot.h"
#include "config.h"

struct UploadTask;
struct SceneData;
class BufferManager;
class ObjectManager;
class Camera;

struct LightCamera {
    glm::mat4 view;
    glm::mat4 proj;
};

class LightDataModule {
public:
    LightDataModule();
    uint32_t CalculateLightSize(ObjectManager* om, SceneData* scene);
    void StoreLightData(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene);

    // Считает размер буфера LightCameras слота И пишет слепок его теневых камер
    // (snapshots[slot]) — одним перечислением, тем же порядком spot→sphere→direct, каким
    // StoreLightCameras наполняет буфер. Size-фаза выполняется ВСЕГДА (в отличие от store,
    // который скипается при size==0), поэтому слепок слота не бывает стейлым.
    // Слепок теневых камер слота. Пишется в СВОЕЙ фазе PrepareFunc (Engine), а не в size_fn:
    // size-функции обязаны быть читалками, иначе порядок регистрации инструкций становится
    // несущим — на этом уже стоял отдельный инвариант.
    void StampShadowCameras(ObjectManager* om, SceneData* scene, uint8_t slot);
    uint32_t CalculateLightCamerasSize(uint8_t slot) const;
    void StoreLightCameras(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene);

    // ── Ask*(slot): читают ТОЛЬКО слепок слота — безопасны с рендер-потока (и с sim после
    // size-фазы LIGHT_CAMERA_BUFFER этого prepare; порядок регистрации апдейтеров это даёт).
    // Отсутствие ObjectManager в параметрах — контракт: в ECS отсюда не ходим.
    uint32_t AskNumLightCameras(uint8_t slot) const {
        return static_cast<uint32_t>(snapshots[slot].cams.size());
    }
    const std::vector<RenderSnap::ShadowCam>& AskShadowCameras(uint8_t slot) const {
        return snapshots[slot].cams;
    }
    bool IsShadowLayerDirty(uint8_t slot, uint32_t layer) const {
        const auto& cams = snapshots[slot].cams;
        return layer < cams.size() && cams[layer].needs_render != 0;
    }

private:
    uint32_t total_size = 0;
    RenderSnap::LightCams snapshots[BUFFERING_LEVEL];
};
