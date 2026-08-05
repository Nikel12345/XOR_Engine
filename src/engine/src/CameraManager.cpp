#include "PCH.h"
#include "CameraManager.h"
#include "BufferManager.h"

Camera* CameraManager::CreateCamera(float width, float height, float fov_y, float near_plane, float far_plane)
{
    cameras.emplace_back(width, height, fov_y, near_plane, far_plane);

    return &cameras.back();
}

void CameraManager::SetActiveCamera(int index)
{
    for (size_t i = 0; i < cameras.size(); ++i) {
        cameras[i].active = false;
    }
	cameras[index].active = true;
}

Camera* CameraManager::GetActiveCamera()
{
    for (size_t i = 0; i < cameras.size(); ++i) {
        if (cameras[i].active) {
            return &cameras[i];
		}
	}
    SDL_Log("No active camera");
	return nullptr;
}

uint32_t CameraManager::CalculateCameraSize()
{
	return sizeof(CameraData);
}

void CameraManager::StoreActiveCamera(BufferManager* bm, UploadTask* task)
{
    Camera* cam = GetActiveCamera();

    CameraData data;
    data.view = cam->GetView();
    data.proj = cam->GetProj();

	bm->UploadToTransferBuffer(task, sizeof(CameraData), &data);
}
