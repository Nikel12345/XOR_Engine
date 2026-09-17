#pragma once
#include "GpuQueues.h"

class QueueManager {
public:
    explicit QueueManager(SDL_GPUDevice* device);

    UploadQueue  GetUploadQueue() const;
    ComputeQueue GetComputeQueue() const;
    RenderQueue  GetRenderQueue() const;

    SDL_GPUDevice* GetDevice() const { return device_; }

private:
    SDL_GPUDevice* device_ = nullptr;
};
