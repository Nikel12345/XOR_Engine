#pragma once
#include <SDL3/SDL_gpu.h>


class UploadCopyPass {
public:
    explicit UploadCopyPass(SDL_GPUCopyPass* cp) : cp_(cp) {}

    void UploadToBuffer(const SDL_GPUTransferBufferLocation* source,
                        const SDL_GPUBufferRegion* destination, bool cycle) const
    { SDL_UploadToGPUBuffer(cp_, source, destination, cycle); }

    void DownloadFromBuffer(const SDL_GPUBufferRegion* source,
                            const SDL_GPUTransferBufferLocation* destination) const
    { SDL_DownloadFromGPUBuffer(cp_, source, destination); }

    void CopyBufferToBuffer(const SDL_GPUBufferLocation* source,
                            const SDL_GPUBufferLocation* destination, Uint32 size, bool cycle) const
    { SDL_CopyGPUBufferToBuffer(cp_, source, destination, size, cycle); }

    void End() const { SDL_EndGPUCopyPass(cp_); }

    explicit operator bool() const { return cp_ != nullptr; }
    SDL_GPUCopyPass* Raw() const { return cp_; }

private:
    SDL_GPUCopyPass* cp_ = nullptr;
};

class TextureCopyPass {
public:
    explicit TextureCopyPass(SDL_GPUCopyPass* cp) : cp_(cp) {}

    void UploadToTexture(const SDL_GPUTextureTransferInfo* source,
                         const SDL_GPUTextureRegion* destination, bool cycle) const
    { SDL_UploadToGPUTexture(cp_, source, destination, cycle); }

    void DownloadFromTexture(const SDL_GPUTextureRegion* source,
                             const SDL_GPUTextureTransferInfo* destination) const
    { SDL_DownloadFromGPUTexture(cp_, source, destination); }

    void CopyTextureToTexture(const SDL_GPUTextureLocation* source,
                              const SDL_GPUTextureLocation* destination,
                              Uint32 w, Uint32 h, Uint32 d, bool cycle) const
    { SDL_CopyGPUTextureToTexture(cp_, source, destination, w, h, d, cycle); }

    void End() const { SDL_EndGPUCopyPass(cp_); }

    explicit operator bool() const { return cp_ != nullptr; }
    SDL_GPUCopyPass* Raw() const { return cp_; }

private:
    SDL_GPUCopyPass* cp_ = nullptr;
};

// ─────────────────────────────────────── командные буферы ─────────────────────────────────

// Командный буфер заливочного лейна. Ни свопчейна, ни мипов, ни блитов, ни пушей — на
// копировальной очереди всего этого не исполнить.
class UploadCommandBuffer {
public:
    explicit UploadCommandBuffer(SDL_GPUCommandBuffer* cb) : cb_(cb) {}

    UploadCopyPass BeginBufferCopyPass() const { return UploadCopyPass(SDL_BeginGPUCopyPass(cb_)); }

    // Два варианта закрытия: без fence — «отправил и забыл», с fence — когда нужен сигнал
    // завершения работы над слотом (SlotController). Отменённый буфер возвращается в пул,
    // ничего не исполнив.
    bool          Submit() const                { return SDL_SubmitGPUCommandBuffer(cb_); }
    SDL_GPUFence* SubmitAndAcquireFence() const { return SDL_SubmitGPUCommandBufferAndAcquireFence(cb_); }
    bool          Cancel() const                { return SDL_CancelGPUCommandBuffer(cb_); }

    explicit operator bool() const { return cb_ != nullptr; }
    SDL_GPUCommandBuffer* Raw() const { return cb_; }

private:
    SDL_GPUCommandBuffer* cb_ = nullptr;
};


class ComputeCommandBuffer {
public:
    explicit ComputeCommandBuffer(SDL_GPUCommandBuffer* cb) : cb_(cb) {}

    UploadCopyPass BeginBufferCopyPass() const { return UploadCopyPass(SDL_BeginGPUCopyPass(cb_)); }

    SDL_GPUComputePass* BeginComputePass(const SDL_GPUStorageTextureReadWriteBinding* storage_texture_bindings,
                                         Uint32 num_storage_texture_bindings,
                                         const SDL_GPUStorageBufferReadWriteBinding* storage_buffer_bindings,
                                         Uint32 num_storage_buffer_bindings) const
    { return SDL_BeginGPUComputePass(cb_, storage_texture_bindings, num_storage_texture_bindings,
                                     storage_buffer_bindings, num_storage_buffer_bindings); }

    void PushComputeUniformData(Uint32 slot, const void* data, Uint32 length) const
    { SDL_PushGPUComputeUniformData(cb_, slot, data, length); }

    bool          Submit() const                { return SDL_SubmitGPUCommandBuffer(cb_); }
    SDL_GPUFence* SubmitAndAcquireFence() const { return SDL_SubmitGPUCommandBufferAndAcquireFence(cb_); }
    bool          Cancel() const                { return SDL_CancelGPUCommandBuffer(cb_); }

    explicit operator bool() const { return cb_ != nullptr; }
    SDL_GPUCommandBuffer* Raw() const { return cb_; }

private:
    SDL_GPUCommandBuffer* cb_ = nullptr;
};


class RenderCommandBuffer {
public:
    explicit RenderCommandBuffer(SDL_GPUCommandBuffer* cb) : cb_(cb) {}

    TextureCopyPass BeginTextureCopyPass() const { return TextureCopyPass(SDL_BeginGPUCopyPass(cb_)); }

    SDL_GPURenderPass* BeginRenderPass(const SDL_GPUColorTargetInfo* color_target_infos,
                                       Uint32 num_color_targets,
                                       const SDL_GPUDepthStencilTargetInfo* depth_stencil_target_info) const
    { return SDL_BeginGPURenderPass(cb_, color_target_infos, num_color_targets, depth_stencil_target_info); }

    SDL_GPUComputePass* BeginComputePass(const SDL_GPUStorageTextureReadWriteBinding* storage_texture_bindings,
                                         Uint32 num_storage_texture_bindings,
                                         const SDL_GPUStorageBufferReadWriteBinding* storage_buffer_bindings,
                                         Uint32 num_storage_buffer_bindings) const
    { return SDL_BeginGPUComputePass(cb_, storage_texture_bindings, num_storage_texture_bindings,
                                     storage_buffer_bindings, num_storage_buffer_bindings); }

    bool AcquireSwapchainTexture(SDL_Window* window, SDL_GPUTexture** swapchain_texture,
                                 Uint32* width, Uint32* height) const
    { return SDL_AcquireGPUSwapchainTexture(cb_, window, swapchain_texture, width, height); }

    void GenerateMipmaps(SDL_GPUTexture* texture) const { SDL_GenerateMipmapsForGPUTexture(cb_, texture); }
    void BlitTexture(const SDL_GPUBlitInfo* info) const { SDL_BlitGPUTexture(cb_, info); }

    void PushVertexUniformData(Uint32 slot, const void* data, Uint32 length) const
    { SDL_PushGPUVertexUniformData(cb_, slot, data, length); }
    void PushFragmentUniformData(Uint32 slot, const void* data, Uint32 length) const
    { SDL_PushGPUFragmentUniformData(cb_, slot, data, length); }
    void PushComputeUniformData(Uint32 slot, const void* data, Uint32 length) const
    { SDL_PushGPUComputeUniformData(cb_, slot, data, length); }

    bool          Submit() const                { return SDL_SubmitGPUCommandBuffer(cb_); }
    SDL_GPUFence* SubmitAndAcquireFence() const { return SDL_SubmitGPUCommandBufferAndAcquireFence(cb_); }
    bool          Cancel() const                { return SDL_CancelGPUCommandBuffer(cb_); }

    explicit operator bool() const { return cb_ != nullptr; }
    SDL_GPUCommandBuffer* Raw() const { return cb_; }

private:
    SDL_GPUCommandBuffer* cb_ = nullptr;
};


class UploadQueue {
public:
    explicit UploadQueue(SDL_GPUDevice* device) : device_(device) {}
    UploadCommandBuffer AcquireCommandBuffer() const
    { return UploadCommandBuffer(SDL_AcquireGPUCommandBufferOnQueue(device_, SDL_GPU_QUEUETYPE_TRANSFER)); }

private:
    SDL_GPUDevice* device_ = nullptr;
};

class ComputeQueue {
public:
    explicit ComputeQueue(SDL_GPUDevice* device) : device_(device) {}
    ComputeCommandBuffer AcquireCommandBuffer() const
    { return ComputeCommandBuffer(SDL_AcquireGPUCommandBufferOnQueue(device_, SDL_GPU_QUEUETYPE_COMPUTE)); }

private:
    SDL_GPUDevice* device_ = nullptr;
};

class RenderQueue {
public:
    explicit RenderQueue(SDL_GPUDevice* device) : device_(device) {}
    RenderCommandBuffer AcquireCommandBuffer() const
    { return RenderCommandBuffer(SDL_AcquireGPUCommandBufferOnQueue(device_, SDL_GPU_QUEUETYPE_GRAPHICS)); }

private:
    SDL_GPUDevice* device_ = nullptr;
};
