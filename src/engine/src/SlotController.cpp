#include "PCH.h"
#include "SlotController.h"

SlotController::SlotController()
    : last_rendering_slot(INVALID_SLOT)
{
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        slots_data[i].frame_id = 0;
        slots_data[i].flags = 0;
        slots_data[i].upload.Clear();
        slots_data[i].render.Clear();
    }
}

SlotController::~SlotController() = default;

uint8_t SlotController::AcquireFreeSlotUnsafe(bool allow_frame_skip)
{
    // Жертва при skip'е выбирается по frame_id через ОБА готовых состояния: предпочесть
    // PREPARED ради экономии вложенной работы нельзя, порядок кадров важнее.
    uint8_t oldest_ready = INVALID_SLOT;

    for (uint8_t offset = 0; offset < BUFFERING_LEVEL; ++offset) {
        uint8_t i = static_cast<uint8_t>(
            (next_free_slot_index + offset) % BUFFERING_LEVEL);
        uint8_t f = slots_data[i].flags;

        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING |
                 SLOT_FLAG_IS_COMPUTING | SLOT_FLAG_IS_RENDERING))
            continue;
        if (BUFFERING_LEVEL > 1 && i == last_rendering_slot)
            continue;

        if (f & (SLOT_FLAG_HAS_PREPARED | SLOT_FLAG_HAS_COMPUTED)) {
            if (allow_frame_skip &&
                (oldest_ready == INVALID_SLOT ||
                 slots_data[i].frame_id < slots_data[oldest_ready].frame_id))
                oldest_ready = i;
            continue;
        }

        slots_data[i].flags = SLOT_FLAG_RESERVED;
        next_free_slot_index = static_cast<uint8_t>((i + 1) % BUFFERING_LEVEL);
        return i;
    }

    if (oldest_ready != INVALID_SLOT) {
        slots_data[oldest_ready].flags = SLOT_FLAG_RESERVED;
        next_free_slot_index = static_cast<uint8_t>((oldest_ready + 1) % BUFFERING_LEVEL);
        return oldest_ready;
    }
    return INVALID_SLOT;
}

uint8_t SlotController::GetFreeSlotIndex(bool allow_frame_skip)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return AcquireFreeSlotUnsafe(allow_frame_skip);
}

uint8_t SlotController::WaitFreeSlotIndex(bool allow_frame_skip)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        // Останов — ДО попытки взять слот: разбуженный один раз поток иначе уснёт навсегда
        // (будить больше некому), и join sim-потока повиснет.
        if (shutting_down_.load(std::memory_order_acquire))
            return INVALID_SLOT;

        uint8_t slot = AcquireFreeSlotUnsafe(allow_frame_skip);
        if (slot != INVALID_SLOT)
            return slot;

        cv_free_.wait(lock);
    }
}

// Снятие готовности с ЧУЖИХ слотов происходит только здесь (см. docs/internals/frame.md).
uint8_t SlotController::GetComputableSlotUnsafe(bool latest_wins)
{
    uint8_t best = INVALID_SLOT;
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        uint8_t f = slots_data[i].flags;
        if (!(f & SLOT_FLAG_HAS_PREPARED))
            continue;
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING |
                 SLOT_FLAG_IS_COMPUTING | SLOT_FLAG_IS_RENDERING))
            continue;
        if (slots_data[i].epoch != required_epoch_)
            continue;
        if (best == INVALID_SLOT ||
            ( latest_wins && slots_data[i].frame_id > slots_data[best].frame_id) ||
            (!latest_wins && slots_data[i].frame_id < slots_data[best].frame_id))
            best = i;
    }
    if (latest_wins && best != INVALID_SLOT) {
        for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
            if (i == best) continue;
            if ((slots_data[i].flags & SLOT_FLAG_HAS_PREPARED) &&
                slots_data[i].frame_id < slots_data[best].frame_id)
                slots_data[i].flags &= static_cast<uint8_t>(~SLOT_FLAG_HAS_PREPARED);
        }
    }
    return best;
}

uint8_t SlotController::GetReadySlotUnsafe(bool latest_wins)
{
    uint8_t best = INVALID_SLOT;
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        uint8_t f = slots_data[i].flags;
        if (!(f & SLOT_FLAG_HAS_COMPUTED))
            continue;
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING |
                 SLOT_FLAG_IS_COMPUTING | SLOT_FLAG_IS_RENDERING))
            continue;
        if (slots_data[i].epoch != required_epoch_)
            continue;
        if (best == INVALID_SLOT ||
            ( latest_wins && slots_data[i].frame_id > slots_data[best].frame_id) ||
            (!latest_wins && slots_data[i].frame_id < slots_data[best].frame_id))
            best = i;
    }
    return best;
}

uint8_t SlotController::GetRenderableFallbackUnsafe()
{
    uint8_t lr = last_rendering_slot;
    if (lr == INVALID_SLOT)
        return INVALID_SLOT;

    // RESERVED/UPLOADING на lr невозможны (sim его не берёт) — проверки защитные.
    if (slots_data[lr].flags &
        (SLOT_FLAG_IS_RENDERING | SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING))
        return INVALID_SLOT;

    if (slots_data[lr].epoch != required_epoch_)
        return INVALID_SLOT;

    return lr;
}

// last_rendering_slot здесь не трогаем: fallback — понятие рендера.
void SlotController::MarkComputingUnsafe(uint8_t slot)
{
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_COMPUTING) & ~SLOT_FLAG_HAS_PREPARED);
}

void SlotController::MarkRenderingUnsafe(uint8_t slot)
{
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_RENDERING) & ~SLOT_FLAG_HAS_COMPUTED);

    if (last_rendering_slot != slot) {
        last_rendering_slot = slot;
        cv_free_.notify_all();
    }
}

uint8_t SlotController::WaitComputableSlot(bool latest_wins)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (shutting_down_.load(std::memory_order_acquire))
            return INVALID_SLOT;

        uint8_t slot = GetComputableSlotUnsafe(latest_wins);
        if (slot != INVALID_SLOT) {
            MarkComputingUnsafe(slot);
            return slot;
        }
        cv_computable_.wait(lock);
    }
}

void SlotController::NotifyShutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_.store(true, std::memory_order_release);
    }
    cv_computable_.notify_all();
    cv_renderable_.notify_all();
    cv_free_.notify_all();
}

uint8_t SlotController::WaitRenderableSlot(bool latest_wins)
{
    std::unique_lock<std::mutex> lock(mutex_);

    constexpr auto SOFT_WAIT = std::chrono::milliseconds(2);

    for (;;) {
        // Та же причина, что в WaitFreeSlotIndex.
        if (shutting_down_.load(std::memory_order_acquire))
            return INVALID_SLOT;

        uint8_t slot = GetReadySlotUnsafe(latest_wins);
        if (slot != INVALID_SLOT) {
            MarkRenderingUnsafe(slot);
            stat_fresh_.fetch_add(1, std::memory_order_relaxed);
            return slot;
        }

        uint8_t fb = GetRenderableFallbackUnsafe();
        if (fb != INVALID_SLOT) {
            cv_renderable_.wait_for(lock, SOFT_WAIT);

            slot = GetReadySlotUnsafe(latest_wins);
            if (slot != INVALID_SLOT) {
                MarkRenderingUnsafe(slot);
                stat_fresh_after_wait_.fetch_add(1, std::memory_order_relaxed);
                return slot;
            }

            MarkRenderingUnsafe(fb);
            stat_fallback_.fetch_add(1, std::memory_order_relaxed);
            return fb;
        }

        cv_renderable_.wait(lock);
    }
}

SlotController::RenderChoiceStats SlotController::GetRenderChoiceStats() const
{
    return { stat_fresh_.load(std::memory_order_relaxed),
             stat_fresh_after_wait_.load(std::memory_order_relaxed),
             stat_fallback_.load(std::memory_order_relaxed) };
}

bool SlotController::IsUploadingSlot(uint8_t slot)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    return (slots_data[slot].flags & SLOT_FLAG_IS_UPLOADING) != 0;
}

bool SlotController::IsRenderingSlot(uint8_t slot)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    return (slots_data[slot].flags & SLOT_FLAG_IS_RENDERING) != 0;
}

void SlotController::HandleUploading(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // При ОТПРАВКЕ загрузки, а не при её завершении: фенсы приходят не по порядку.
    slots_data[slot].frame_id = ++prepared_seq;
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_RESERVED) | SLOT_FLAG_IS_UPLOADING);
}

void SlotController::HandlePrepared(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_IS_UPLOADING) | SLOT_FLAG_HAS_PREPARED);

    cv_computable_.notify_one();
    cv_free_.notify_all();
}

void SlotController::HandleComputing(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Идемпотентен: те же флаги уже выставил MarkComputingUnsafe при захвате слота.
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_COMPUTING) & ~SLOT_FLAG_HAS_PREPARED);
}

void SlotController::HandleComputed(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_IS_COMPUTING) | SLOT_FLAG_HAS_COMPUTED);

    cv_renderable_.notify_one();
    cv_free_.notify_all();        // в skip-режиме готовый кадр — цель для перезаписи
}

void SlotController::HandleRendered(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].render.Clear();
    slots_data[slot].flags = static_cast<uint8_t>(
        slots_data[slot].flags & ~SLOT_FLAG_IS_RENDERING);

    cv_renderable_.notify_one();  // слот снова годится в fallback
    cv_free_.notify_all();        // и в запись, если он не lr
}

void SlotController::SetSlotState(uint8_t slot, SlotState new_state)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    switch (new_state) {
    case SlotState::UPLOADING: HandleUploading(slot); break;
    case SlotState::PREPARED:  HandlePrepared(slot);  break;
    case SlotState::COMPUTING: HandleComputing(slot); break;
    case SlotState::COMPUTED:  HandleComputed(slot);  break;
    case SlotState::RENDERED:  HandleRendered(slot);  break;
    }
}

void SlotController::PushUploadFence(uint8_t slot, SDL_GPUFence* fence)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].upload.Push(fence);
}

void SlotController::SetRenderFence(uint8_t slot, SDL_GPUFence* fence)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].render.Clear();
    slots_data[slot].render.Push(fence);
}

void SlotController::StampSlotEpoch(uint8_t slot, uint64_t epoch)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].epoch = epoch;
    // Будить никого не надо: планка лишь СУЖАЕТ множество отдаваемых кадров.
    if (epoch > required_epoch_)
        required_epoch_ = epoch;
}

void SlotController::DebugDump(const char* tag)
{
    std::lock_guard<std::mutex> lock(mutex_);

    SDL_Log("==== SlotController::DebugDump %s ====", tag ? tag : "");
    SDL_Log(" last_rendering_slot = %u", last_rendering_slot);

    static_assert(StageFences::CAP == 2, "формат строки ниже печатает ровно два upload-fence");
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        const SlotData& sd = slots_data[i];
        uint8_t f = sd.flags;
        SDL_Log(
            " slot %u: flags=0x%02X [Rsv=%u U=%u P=%u R=%u], frame_id=%u, upload=%u{%p,%p}, render=%p",
            i,
            f,
            (f & SLOT_FLAG_RESERVED) != 0,
            (f & SLOT_FLAG_IS_UPLOADING) != 0,
            (f & SLOT_FLAG_HAS_PREPARED) != 0,
            (f & SLOT_FLAG_IS_RENDERING) != 0,
            sd.frame_id,
            sd.upload.count,
            (void*)sd.upload.items[0],
            (void*)sd.upload.items[1],
            (void*)sd.render.items[0]
        );
    }
    SDL_Log("======================================");
}
