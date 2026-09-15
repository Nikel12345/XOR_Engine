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
    void StampShadowCameras(ObjectManager* om, SceneData* scene, uint8_t slot);
    uint32_t CalculateLightCamerasSize(uint8_t slot) const;
    void StoreLightCameras(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene);

    uint32_t AskNumLightCameras(uint8_t slot) const {
        return static_cast<uint32_t>(snapshots[slot].cams.size());
    }
    uint32_t AskNumLights(uint8_t slot) const { return snapshots[slot].num_lights; }
    const std::vector<RenderSnap::ShadowCam>& AskShadowCameras(uint8_t slot) const {
        return snapshots[slot].cams;
    }

    // Занят ли слой теневого массива камерой в ЭТОМ слоте. Слоёв в атласе фиксированное число, а
    // камер столько, сколько дала сцена, поэтому хвост слоёв держит мусор прошлой сцены — его не
    // читают (проход рисует только num_cams слоёв) и обрабатывать не должны.
    bool IsShadowLayerUsed(uint8_t slot, uint32_t layer) const {
        return layer < snapshots[slot].cams.size();
    }

private:
    uint32_t total_size = 0;
    RenderSnap::LightCams snapshots[BUFFERING_LEVEL];
};
