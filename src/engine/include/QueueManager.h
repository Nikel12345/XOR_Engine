#pragma once
#include "GpuQueues.h"

// Владелец очередей GPU и единственная точка, где решается, на какую из них поедет лейн.
//
// Лист, как и прочие менеджеры (см. CLAUDE.md): не держит указателей на другие менеджеры и не
// зовёт их. Наружу отдаёт ЗНАЧЕНИЯ (UploadQueue/RenderQueue размером с указатель) — их не
// нужно хранить в полях, берутся заново там, где нужны.
//
// СЕГОДНЯ обе очереди — одна и та же: SDL_GPU однооочередной во всех бэкендах (Vulkan
// unifiedQueue, D3D12 один DIRECT-список, Metal один MTLCommandQueue), и отдельной очереди
// заливки просто нет откуда взять. Это НЕ временная заглушка, а штатный режим fallback: он
// останется рабочим навсегда — для Metal и для железа, где второй семьи очередей нет.
// Разделение на типы от этого не теряет смысла: оно фиксирует НАМЕРЕНИЕ вызывающего, и когда
// вторая очередь появится, ни одна строка вызывающего кода не поменяется — поменяется только
// то, что вернёт GetUploadQueue.
class QueueManager {
public:
    explicit QueueManager(SDL_GPUDevice* device);

    UploadQueue GetUploadQueue() const;
    RenderQueue GetRenderQueue() const;

    // Есть ли на устройстве ОТДЕЛЬНАЯ очередь заливки. false — режим fallback: обе обёртки
    // смотрят в одну очередь, порядок команд между лейнами задаётся порядком сабмитов.
    // true — лейны исполняются параллельно, и упорядочивать их обязан вызывающий (в этом
    // движке это делает fence слота, см. Engine::UploadFunc/FenceFunc).
    bool HasDedicatedUploadQueue() const { return has_dedicated_upload_queue_; }

    SDL_GPUDevice* GetDevice() const { return device_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    bool has_dedicated_upload_queue_ = false;
};
