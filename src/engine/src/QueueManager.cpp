#include "PCH.h"
#include "QueueManager.h"

QueueManager::QueueManager(SDL_GPUDevice* device)
    : device_(device)
{
}

UploadQueue  QueueManager::GetUploadQueue() const  { return UploadQueue(device_); }
ComputeQueue QueueManager::GetComputeQueue() const { return ComputeQueue(device_); }
RenderQueue  QueueManager::GetRenderQueue() const  { return RenderQueue(device_); }
