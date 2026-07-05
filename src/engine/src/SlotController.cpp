#include "PCH.h"
#include "SlotController.h"

SlotController::SlotController()
    : last_rendering_slot(INVALID_SLOT)
{
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        slots_data[i].frame_id = 0;
        slots_data[i].flags = 0;
        slots_data[i].fence = nullptr;
    }
}

SlotController::~SlotController() = default;

// ====================== Sim: захват слота под prepare ======================

uint8_t SlotController::AcquireFreeSlotUnsafe(bool allow_frame_skip)
{
    // Два прохода по логической очереди готовности:
    //   1) слоты без флагов — их содержимое уже показано или дропнуто («вышли из
    //      очереди»), ничего ценного не теряем;
    //   2) только при skip'е: САМЫЙ СТАРЫЙ PREPARED — наименее ценный кадр.
    //      Перезаписать «любой» нельзя: убив свежайший, рендер покажет более
    //      старый — визуальный откат.
    uint8_t oldest_prepared = INVALID_SLOT;

    for (uint8_t offset = 0; offset < BUFFERING_LEVEL; ++offset) {
        uint8_t i = static_cast<uint8_t>(
            (next_free_slot_index + offset) % BUFFERING_LEVEL);
        uint8_t f = slots_data[i].flags;

        // Нельзя писать в то, что использует GPU: загрузка в полёте,
        // рендерящийся кадр и fallback-источник.
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING | SLOT_FLAG_IS_RENDERING))
            continue;
        if (BUFFERING_LEVEL > 1 && i == last_rendering_slot)
            continue;

        if (f & SLOT_FLAG_HAS_PREPARED) {
            if (allow_frame_skip &&
                (oldest_prepared == INVALID_SLOT ||
                 slots_data[i].frame_id < slots_data[oldest_prepared].frame_id))
                oldest_prepared = i;
            continue;
        }

        // Слот без флагов — берём сразу.
        slots_data[i].flags = SLOT_FLAG_RESERVED;
        next_free_slot_index = static_cast<uint8_t>((i + 1) % BUFFERING_LEVEL);
        return i;
    }

    if (oldest_prepared != INVALID_SLOT) {
        // Frame skip: неотрисованный кадр молча перезаписывается более свежим.
        // Снятие PREPARED атомарно с резервацией — рендер не возьмёт слот,
        // пока sim заливает его буферы.
        slots_data[oldest_prepared].flags = SLOT_FLAG_RESERVED;
        next_free_slot_index = static_cast<uint8_t>((oldest_prepared + 1) % BUFFERING_LEVEL);
        return oldest_prepared;
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
        uint8_t slot = AcquireFreeSlotUnsafe(allow_frame_skip);
        if (slot != INVALID_SLOT)
            return slot;

        // Разбудят: HandleRendered (слот вышел из рендера), MarkRenderingUnsafe
        // (lr переехал — старый fallback-слот стал записываемым) и HandlePrepared
        // (в skip-режиме завершившаяся загрузка — цель для перезаписи).
        cv_free_.wait(lock);
    }
}

// ====================== Render: выбор кадра ================================

// latest_wins (skip-режим): СВЕЖАЙШИЙ PREPARED; более старые кандидаты при этом
// скипаются насовсем (PREPARED снимается) — показывать кадр старее выбранного
// нельзя, а sim перезапишет их первыми. Без latest_wins (lockstep): СТАРЕЙШИЙ,
// по порядку и без потерь — каждый подготовленный кадр обязан быть показан.
uint8_t SlotController::GetReadySlotUnsafe(bool latest_wins)
{
    uint8_t best = INVALID_SLOT;
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        uint8_t f = slots_data[i].flags;
        if (!(f & SLOT_FLAG_HAS_PREPARED))
            continue;
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING | SLOT_FLAG_IS_RENDERING))
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

uint8_t SlotController::GetRenderableFallbackUnsafe()
{
    uint8_t lr = last_rendering_slot;
    if (lr == INVALID_SLOT)
        return INVALID_SLOT;

    // Пока кадр в полёте, пере-рендерить его слот нельзя. RESERVED/UPLOADING на lr
    // невозможны (sim не берёт lr), проверки защитные.
    if (slots_data[lr].flags &
        (SLOT_FLAG_IS_RENDERING | SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING))
        return INVALID_SLOT;

    return lr;
}

// Пометка «ушёл на GPU» ставится в момент ВЫБОРА слота (под mutex_), а не после
// сабмита: в окне между выбором и сабмитом sim не должен захватить слот на запись.
// last_rendering_slot — последний отправленный; fallback пользуется им только
// после снятия IS_RENDERING (его снимет FenceThread по завершении fence).
void SlotController::MarkRenderingUnsafe(uint8_t slot)
{
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_RENDERING) & ~SLOT_FLAG_HAS_PREPARED);

    if (last_rendering_slot != slot) {
        last_rendering_slot = slot;
        cv_free_.notify_all();  // старый lr перестал быть fallback-источником — sim может писать
    }
}

uint8_t SlotController::WaitRenderableSlot(bool latest_wins)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // Мягкий бюджет ожидания нового кадра при живом fallback. Сам fallback —
    // задел на будущее (пере-рендер последнего кадра с per-render данными:
    // время в шейдерах, камера на частоте рендера); сейчас он даёт идентичный кадр.
    constexpr auto SOFT_WAIT = std::chrono::milliseconds(2);

    for (;;) {
        // 1. Новый готовый кадр — берём сразу.
        uint8_t slot = GetReadySlotUnsafe(latest_wins);
        if (slot != INVALID_SLOT) {
            MarkRenderingUnsafe(slot);
            return slot;
        }

        // 2. Есть что показать «по-старому» — но сначала чуть подождём новый кадр.
        uint8_t fb = GetRenderableFallbackUnsafe();
        if (fb != INVALID_SLOT) {
            cv_renderable_.wait_for(lock, SOFT_WAIT);

            slot = GetReadySlotUnsafe(latest_wins);
            if (slot != INVALID_SLOT) {
                MarkRenderingUnsafe(slot);
                return slot;
            }

            MarkRenderingUnsafe(fb);
            return fb;
        }

        // 3. Ни нового кадра, ни fallback (самый первый кадр) — ждём пробуждения.
        cv_renderable_.wait(lock);
    }
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

// ====================== Переходы состояний =================================

void SlotController::HandleUploading(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Номер кадра — порядок sim-тиков: ставится при ОТПРАВКЕ загрузки, а не при
    // её завершении, чтобы приход fences не по порядку не переупорядочил кадры.
    slots_data[slot].frame_id = ++prepared_seq;
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_RESERVED) | SLOT_FLAG_IS_UPLOADING);
    // Никого не будим: слот стал НЕдоступнее (был RESERVED, стал UPLOADING).
}

void SlotController::HandlePrepared(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_IS_UPLOADING) | SLOT_FLAG_HAS_PREPARED);

    cv_renderable_.notify_one();  // рендеру появился кадр
    cv_free_.notify_all();        // в skip-режиме готовый кадр — цель для перезаписи
}

void SlotController::HandleRendered(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].fence = nullptr;
    slots_data[slot].flags = static_cast<uint8_t>(
        slots_data[slot].flags & ~SLOT_FLAG_IS_RENDERING);

    cv_renderable_.notify_one();  // fallback стал доступен
    cv_free_.notify_all();        // слот стал записываемым (если он не lr)
}

void SlotController::SetSlotState(uint8_t slot, SlotState new_state)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    switch (new_state) {
    case UPLOADING: HandleUploading(slot); break;
    case PREPARED:  HandlePrepared(slot);  break;
    case RENDERED:  HandleRendered(slot);  break;
    }
}

void SlotController::SetSlotFence(uint8_t slot, SDL_GPUFence* fence)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].fence = fence;
}

// ====================== Отладка ============================================

void SlotController::DebugDump(const char* tag)
{
    std::lock_guard<std::mutex> lock(mutex_);

    SDL_Log("==== SlotController::DebugDump %s ====", tag ? tag : "");
    SDL_Log(" last_rendering_slot = %u", last_rendering_slot);

    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        const SlotData& sd = slots_data[i];
        uint8_t f = sd.flags;
        SDL_Log(
            " slot %u: flags=0x%02X [Rsv=%u U=%u P=%u R=%u], frame_id=%u, fence=%p",
            i,
            f,
            (f & SLOT_FLAG_RESERVED) != 0,
            (f & SLOT_FLAG_IS_UPLOADING) != 0,
            (f & SLOT_FLAG_HAS_PREPARED) != 0,
            (f & SLOT_FLAG_IS_RENDERING) != 0,
            sd.frame_id,
            (void*)sd.fence
        );
    }
    SDL_Log("======================================");
}
