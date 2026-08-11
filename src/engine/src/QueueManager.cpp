#include "PCH.h"
#include "QueueManager.h"

QueueManager::QueueManager(SDL_GPUDevice* device)
    : device_(device)
{
    // Пока форк SDL не умеет заводить вторую очередь, определять нечего: она одна.
    // Когда умение появится, здесь встанет запрос к SDL («досталась ли отдельная семья»),
    // а не проверка железа своими силами — семьи очередей видит только бэкенд.
    has_dedicated_upload_queue_ = false;
}

// Обе очереди сегодня — одна и та же (см. HasDedicatedUploadQueue). Возвращаем разные ТИПЫ
// поверх одного устройства: разделение живёт в системе типов, а не в железе, и потому работает
// одинаково в обоих режимах.
UploadQueue QueueManager::GetUploadQueue() const { return UploadQueue(device_); }
RenderQueue QueueManager::GetRenderQueue() const { return RenderQueue(device_); }
