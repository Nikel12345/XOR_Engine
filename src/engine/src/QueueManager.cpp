#include "PCH.h"
#include "QueueManager.h"

QueueManager::QueueManager(SDL_GPUDevice* device)
    : device_(device)
{
}

// Все три отдают ОДНО И ТО ЖЕ устройство — разделение живёт в системе типов и в роли, которую
// обёртка подставляет в SDL_AcquireGPUCommandBufferOnQueue. Куда роль в итоге поедет, знает
// только SDL: он и выбирал семьи. Спрашивать его об этом здесь незачем — вырождение ролей в
// одну очередь ничего в этом коде не меняет.
UploadQueue  QueueManager::GetUploadQueue() const  { return UploadQueue(device_); }
ComputeQueue QueueManager::GetComputeQueue() const { return ComputeQueue(device_); }
RenderQueue  QueueManager::GetRenderQueue() const  { return RenderQueue(device_); }
