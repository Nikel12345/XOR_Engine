#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "CameraStruct.h"

struct UploadTask;
struct BufferData;
class BufferManager;

class CameraManager {
public:
    CameraManager() = default;
    Camera* CreateCamera(float width, float height, float fov_y = Camera::DEFAULT_FOV_Y, float near_plane = Camera::DEFAULT_NEAR_PLANE, float far_plane = Camera::DEFAULT_FAR_PLANE);
    const Camera& GetCamera(int index) const { return cameras[index]; }
    void SetActiveCamera(int index);
    Camera* GetActiveCamera();
    
    uint32_t CalculateCameraSize();
    void StoreActiveCamera(BufferManager* bm, UploadTask* task);
    void StoreActiveCamera(BufferManager* bm, SDL_GPUCopyPass* cp);

private:
    std::vector<Camera> cameras;
};