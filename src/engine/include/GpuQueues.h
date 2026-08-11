#pragma once
#include <SDL3/SDL_gpu.h>

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Типы-обёртки над очередями GPU: заливка и рендер разведены на уровне ТИПА.
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// ЗАЧЕМ. SDL_GPU — плоский C-API: любой SDL_GPUCommandBuffer* умеет всё, и ничто не мешает
// записать SDL_CopyGPUTextureToTexture или SDL_GenerateMipmapsForGPUTexture в буфер, который
// поедет на копировальную очередь. Там их нечем исполнить: это отрисовка. Обёртки убирают
// такой вызов на этапе компиляции — до upload-лейна просто не доходят методы, которых на нём
// быть не может.
//
// ИЕРАРХИЯ. Получение идёт цепочкой по концептуальному родителю, а не глобальными функциями:
//
//     QueueManager
//     ├── GetUploadQueue()                  → UploadQueue
//     │     └── AcquireCommandBuffer()      → UploadCommandBuffer
//     │           └── BeginBufferCopyPass() → UploadCopyPass       // ТОЛЬКО буферные операции
//     ├── GetComputeQueue()                 → ComputeQueue
//     │     └── AcquireCommandBuffer()      → ComputeCommandBuffer
//     │           ├── BeginBufferCopyPass() → UploadCopyPass
//     │           └── BeginComputePass(...) → SDL_GPUComputePass*
//     └── GetRenderQueue()                  → RenderQueue
//           └── AcquireCommandBuffer()      → RenderCommandBuffer
//                 ├── BeginTextureCopyPass()  → TextureCopyPass    // + текстурные операции
//                 ├── BeginRenderPass(...)    → SDL_GPURenderPass*
//                 └── BeginComputePass(...)   → SDL_GPUComputePass*
//
// Способности ВЛОЖЕНЫ: upload ⊂ compute ⊂ render. Ни одного метода, который есть на узком
// лейне и отсутствует на широком.
//
// ИМЕНОВАНИЕ разное на каждом уровне, и разница несёт смысл о времени жизни:
//   • Get…    — объект уже существует, живёт со всем устройством, вызов идемпотентен и
//               ничего не должен вызывающему (очередь);
//   • Acquire — АРЕНДА из пула. SDL держит пер-поточный пул командных буферов
//               (VulkanCommandPool::inactiveCommandBuffers): взял дважды — получил два РАЗНЫХ
//               буфера, и каждый обязан быть Submit-нут или Cancel-нут;
//   • Begin…  — не выдача объекта, а РЕЖИМ командного буфера. SDL_BeginGPUCopyPass возвращает
//               указатель на ПОЛЕ самого буфера (SDL_gpu.c: &commandBufferHeader->copy_pass),
//               ничего не аллоцируя; войти во второй пасс, не закрыв первый, — ошибка
//               (CHECK_ANY_PASS_IN_PROGRESS). Отсюда обязательный парный End().
//
// FALLBACK. Разделение на методы существует всегда, независимо от железа. Когда устройство
// даёт одну очередь (сегодня — всегда: SDL однооочередной), обе обёртки смотрят в неё же, и
// команды, записанные этим API, просто идут одной лентой. Вызывающий код при этом не меняется
// ни строкой — в этом и смысл: тип фиксирует НАМЕРЕНИЕ, а куда оно поедет, решает QueueManager.
//
// ГРАНИЦА. Render-/compute-пассы отдаются СЫРЫМИ (SDL_GPURenderPass*). Обёртывать их нутро —
// это переписать всю систему пасс-стэпов и игровые колбэки, а по лейнам не даёт ничего:
// рендер-пасс и так может родиться только у RenderCommandBuffer. Опасность жила на уровне
// командного буфера — типы стоят там.
//
// Raw() — люк для чужого API (ImGui_ImplSDLGPU3_* берёт голый cb). В своём коде звать Raw()
// незачем: всё, что лейну положено, есть методом. Если понадобилось — это или недостающий
// метод (добавь сюда), или операция не на своём лейне (тогда Raw() и есть баг).

// ─────────────────────────────────────────── пассы ────────────────────────────────────────

// Копи-пасс заливочного лейна: буферы и только буферы. Текстурных методов здесь нет намеренно —
// их отсутствие и есть вся защита.
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

// Копи-пасс рендер-лейна: текстурные операции. Именно они требуют возврата ресурса в состояние
// по умолчанию, а у текстуры это РАСКЛАДКА байт, которую копировальная очередь выразить не может
// (см. ENGINE-FORK-комментарий у VULKAN_INTERNAL_TextureMemoryBarrier).
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

// Командный буфер вычислительного лейна. Способности вложены (TRANSFER ⊂ COMPUTE ⊂ GRAPHICS),
// поэтому здесь есть всё, что у заливочного, плюс вычислительный пасс. Чего нет: рендер-пассов,
// мипов, блитов, свопчейна — они отрисовка, на очереди без графического бита их не исполнить.
//
// Текстурного копи-пасса тут НАМЕРЕННО нет, хотя копирования изображений вычислительная очередь
// исполняет. Дело в барьерах: возврат текстуры в состояние по умолчанию выражается стадиями, а
// SAMPLER-состояние (самое частое) называет стадии вершинника и фрагментника, которых на этой
// очереди не существует. Появится вычислительная работа с текстурами — открывать надо не этот
// метод, а сперва вопрос, какие состояния на ней выразимы (см. SDL_FORK.md).
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

// Командный буфер рендер-лейна: всё остальное. Мипы и блит стоят на командном буфере, а не в
// пассе (так их объявляет SDL), поэтому типизировать пассы было бы недостаточно — обе операции
// это отрисовка, и запретить их на заливке можно только здесь.
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

// ────────────────────────────────────────── очереди ───────────────────────────────────────
// Значения размером с указатель: копируются свободно, ничем не владеют, живут сколько угодно —
// сама очередь принадлежит устройству. Хранить их в полях не нужно, берутся у QueueManager.

// Роль передаётся ЗДЕСЬ и только здесь — дальше она едет на самом командном буфере. Поэтому у
// Submit/Cancel аргумента очереди нет и быть не должно: буфер взят из пула конкретной семьи и
// в чужую очередь не отправляется в принципе (SDL сабмитит в commandPool->queue).
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
