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
    for (uint8_t offset = 0; offset < BUFFERING_LEVEL; ++offset) {
        uint8_t i = static_cast<uint8_t>(
            (next_free_slot_index + offset) % BUFFERING_LEVEL);
        uint8_t f = slots_data[i].flags;

        // Нельзя писать в то, что читает GPU: рендерящийся кадр и fallback-источник.
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_RENDERING))
            continue;
        // Без skip'а PREPARED неприкосновенен: каждый подготовленный кадр обязан
        // отрисоваться, sim ждёт, пока рендер его заберёт (UPS проседает до FPS).
        if (!allow_frame_skip && (f & SLOT_FLAG_HAS_PREPARED))
            continue;
        if (BUFFERING_LEVEL > 1 && i == last_rendering_slot)
            continue;

        // Со skip'ом PREPARED не препятствие: неотрисованный кадр молча
        // перезаписывается более свежим. Снятие PREPARED атомарно с резервацией —
        // рендер не возьмёт слот, пока sim заливает его буферы.
        slots_data[i].flags = SLOT_FLAG_RESERVED;
        next_free_slot_index = static_cast<uint8_t>((i + 1) % BUFFERING_LEVEL);
        return i;
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

        // Разбудят HandleRendered (слот вышел из рендера) и MarkRenderingUnsafe
        // (lr переехал и PREPARED ушёл в рендер — слот стал записываемым).
        cv_free_.wait(lock);
    }
}

// ====================== Render: выбор кадра ================================

// Самый свежий PREPARED-слот. Более старые кандидаты скипаются насовсем (PREPARED
// снимается): показывать кадр старее выбранного нельзя, а sim перезапишет их первыми.
uint8_t SlotController::GetReadySlotUnsafe()
{
    uint8_t best = INVALID_SLOT;
    for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
        uint8_t f = slots_data[i].flags;
        if (!(f & SLOT_FLAG_HAS_PREPARED))
            continue;
        if (f & (SLOT_FLAG_RESERVED | SLOT_FLAG_IS_RENDERING))
            continue;
        if (best == INVALID_SLOT || slots_data[i].frame_id > slots_data[best].frame_id)
            best = i;
    }
    if (best != INVALID_SLOT) {
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

    // Пока кадр в полёте, пере-рендерить его слот нельзя. RESERVED на lr невозможен
    // (sim не берёт lr), проверка защитная.
    if (slots_data[lr].flags & (SLOT_FLAG_IS_RENDERING | SLOT_FLAG_RESERVED))
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

uint8_t SlotController::WaitRenderableSlot()
{
    std::unique_lock<std::mutex> lock(mutex_);

    // Мягкий бюджет ожидания нового кадра при живом fallback. Сам fallback —
    // задел на будущее (пере-рендер последнего кадра с per-render данными:
    // время в шейдерах, камера на частоте рендера); сейчас он даёт идентичный кадр.
    constexpr auto SOFT_WAIT = std::chrono::milliseconds(2);

    for (;;) {
        // 1. Новый готовый кадр — берём сразу.
        uint8_t slot = GetReadySlotUnsafe();
        if (slot != INVALID_SLOT) {
            MarkRenderingUnsafe(slot);
            return slot;
        }

        // 2. Есть что показать «по-старому» — но сначала чуть подождём новый кадр.
        uint8_t fb = GetRenderableFallbackUnsafe();
        if (fb != INVALID_SLOT) {
            cv_renderable_.wait_for(lock, SOFT_WAIT);

            slot = GetReadySlotUnsafe();
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

bool SlotController::IsRenderingSlot(uint8_t slot)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    return (slots_data[slot].flags & SLOT_FLAG_IS_RENDERING) != 0;
}

// ====================== Переходы состояний =================================

void SlotController::HandlePrepared(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].frame_id = ++prepared_seq;
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_RESERVED) | SLOT_FLAG_HAS_PREPARED);

    cv_renderable_.notify_one();
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
    case PREPARED: HandlePrepared(slot); break;
    case RENDERED: HandleRendered(slot); break;
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
            " slot %u: flags=0x%02X [Rsv=%u P=%u R=%u], frame_id=%u, fence=%p",
            i,
            f,
            (f & SLOT_FLAG_RESERVED) != 0,
            (f & SLOT_FLAG_HAS_PREPARED) != 0,
            (f & SLOT_FLAG_IS_RENDERING) != 0,
            sd.frame_id,
            (void*)sd.fence
        );
    }
    SDL_Log("======================================");
}
