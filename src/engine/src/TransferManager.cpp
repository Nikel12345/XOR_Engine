#include "PCH.h"
#include "TransferManager.h"

TransferManager::TransferManager(SDL_GPUDevice* device) : dev(device) {}

TransferManager::~TransferManager()
{
    for (auto& tbd : upload_pool)
        if (tbd->tb) SDL_ReleaseGPUTransferBuffer(dev, tbd->tb);
    for (auto& tbd : download_pool)
        if (tbd->tb) SDL_ReleaseGPUTransferBuffer(dev, tbd->tb);
}

// Классы размеров: BASE_TB_SIZE, удваиваемый до нужного. Без округления пул зарастает
// буферами случайных ёмкостей, которые никто больше не переиспользует.
static uint32_t SizeClass(uint32_t size)
{
    uint32_t c = BASE_TB_SIZE;
    while (c < size) c *= 2;
    return c;
}

TransferBufferData* TransferManager::AcquireUploadTB(uint32_t size)
{
    return AcquireTB(upload_pool, size, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
}

TransferBufferData* TransferManager::AcquireDownloadTB(uint32_t size)
{
    return AcquireTB(download_pool, size, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
}

TransferBufferData* TransferManager::AcquireTB(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage)
{
    if (size == 0) return nullptr;

    TransferBufferData* tbd = EnsureTBCapacity(pool, size, usage);   // уже помечен busy
    if (!tbd) return nullptr;

    // Map вне лока: буфер помечен занятым, другой поток его не возьмёт.
    tbd->mapped = SDL_MapGPUTransferBuffer(dev, tbd->tb, false);
    if (!tbd->mapped)
    {
        SDL_Log("Failed to map transfer buffer (%u bytes): %s", tbd->size, SDL_GetError());
        ReleaseTB(tbd);
        return nullptr;
    }
    return tbd;
}

// Возвращает свободную запись >= size, помеченную busy, или создаёт новую. Дорогой драйверный
// SDL_CreateGPUTransferBuffer выполняется ВНЕ лока — под локом только скан пула, установка busy
// и push_back (всё быстрое). Гонка «два потока не нашли свободный и оба создали» безвредна:
// лишний буфер просто пополняет пул и переиспользуется позже.
TransferBufferData* TransferManager::EnsureTBCapacity(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage)
{
    {
        std::lock_guard lock(mtx);
        // Наименьший подходящий свободный — чтобы большие буферы не разбазаривались на мелочь.
        TransferBufferData* best = nullptr;
        for (auto& entry : pool)
            if (!entry->busy && entry->size >= size && (!best || entry->size < best->size))
                best = entry.get();
        if (best) { best->busy = true; return best; }
    }

    // Свободного нет — создаём вне лока (драйверная аллокация может быть медленной).
    SDL_GPUTransferBufferCreateInfo info{};
    info.size = SizeClass(size);
    info.usage = usage;

    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &info);
    if (!tb)
    {
        SDL_Log("Failed to create transfer buffer (%u bytes): %s", info.size, SDL_GetError());
        return nullptr;
    }

    auto entry = std::make_unique<TransferBufferData>();
    entry->tb = tb;
    entry->size = info.size;
    entry->busy = true;   // наш сразу, ещё до попадания в пул
    TransferBufferData* tbd = entry.get();

    std::lock_guard lock(mtx);
    pool.push_back(std::move(entry));
    SDL_Log("TransferManager: new %s transfer buffer %u bytes (pool size %u)",
        usage == SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD ? "upload" : "download",
        info.size, (uint32_t)pool.size());
    return tbd;
}

void TransferManager::ReleaseTB(TransferBufferData* tbd)
{
    if (!tbd) return;

    std::lock_guard lock(mtx);
    if (!tbd->busy)
    {
        SDL_Log("TransferManager: release of a non-acquired transfer buffer");
        return;
    }
    if (tbd->mapped)
    {
        SDL_UnmapGPUTransferBuffer(dev, tbd->tb);
        tbd->mapped = nullptr;
    }
    tbd->busy = false;
}
