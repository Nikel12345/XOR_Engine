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

uint8_t SlotController::AcquireFreeSlotUnsafe(bool allow_frame_skip)
{
    // Два прохода по логической очереди готовности:
    //   1) слоты без флагов — их содержимое уже показано или дропнуто («вышли из
    //      очереди»), ничего ценного не теряем;
    //   2) только при skip'е: САМЫЙ СТАРЫЙ готовый кадр — наименее ценный.
    //      Перезаписать «любой» нельзя: убив свежайший, рендер покажет более
    //      старый — визуальный откат.
    // Готовых состояний ДВА: PREPARED (залит, не посчитан) и COMPUTED (посчитан,
    // не показан). Жертва выбирается по frame_id ЧЕРЕЗ ОБА: экономить вложенную
    // работу, предпочитая PREPARED, нельзя — порядок кадров важнее.
    uint8_t oldest_ready = INVALID_SLOT;

    for (uint8_t offset = 0; offset < BUFFERING_LEVEL; ++offset) {
        uint8_t i = static_cast<uint8_t>(
            (next_free_slot_index + offset) % BUFFERING_LEVEL);
        uint8_t f = slots_data[i].flags;

        // Нельзя писать в то, что использует GPU: загрузка в полёте, вычисление
        // в полёте, рендерящийся кадр и fallback-источник.
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
        // Frame skip: неотрисованный кадр молча перезаписывается более свежим.
        // Снятие флагов готовности атомарно с резервацией — ни compute, ни рендер
        // не возьмут слот, пока sim заливает его буферы.
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
        uint8_t slot = AcquireFreeSlotUnsafe(allow_frame_skip);
        if (slot != INVALID_SLOT)
            return slot;

        // Разбудят: HandleRendered (слот вышел из рендера), MarkRenderingUnsafe
        // (lr переехал — старый fallback-слот стал записываемым) и HandlePrepared
        // (в skip-режиме завершившаяся загрузка — цель для перезаписи).
        cv_free_.wait(lock);
    }
}

// ВЫЧИСЛИТЕЛЬНАЯ СТАДИЯ — ЕДИНСТВЕННЫЙ ВЛАДЕЛЕЦ УСТАРИВАНИЯ.
// latest_wins (skip-режим): СВЕЖАЙШИЙ PREPARED; более старые кандидаты скипаются
// насовсем (PREPARED снимается) — считать и показывать кадр старее выбранного нельзя,
// а sim перезапишет их первыми. Без latest_wins (lockstep): СТАРЕЙШИЙ, по порядку и
// без потерь — каждый подготовленный кадр обязан пройти обе стадии.
//
// Скип переехал СЮДА из выбора рендера, и это не косметика: раньше стадия компьюта
// честно считала кадры, которые рендер потом выбрасывал, то есть жгла GPU впустую.
// Плюс разрушительная правка чужих слотов теперь ровно в одном месте.
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
        // Эпохо-гейт стоит и здесь, и у рендера. Здесь — чтобы не считать слот, который
        // рендер всё равно отвергнет (чистая экономия GPU); у рендера — потому что эпоха
        // может скакнуть уже ПОСЛЕ вычисления.
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

// Рендеру достаётся УЖЕ ОТФИЛЬТРОВАННОЕ вычислительной стадией: он только ВЫБИРАЕТ по
// frame_id (свежайший при skip, старейший при lockstep) и чужих слотов не трогает.
// Цикла снятия флагов здесь больше нет — он переехал в GetComputableSlotUnsafe.
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
        // Слот старой эпохи ребилда: его indirect/out_pib собраны под прежней раскладкой
        // дерева, а рендер читает уже перестроенное дерево → не показываем (ждём свежий).
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

    // Пока кадр в полёте, пере-рендерить его слот нельзя. RESERVED/UPLOADING на lr
    // невозможны (sim не берёт lr), проверки защитные.
    if (slots_data[lr].flags &
        (SLOT_FLAG_IS_RENDERING | SLOT_FLAG_RESERVED | SLOT_FLAG_IS_UPLOADING))
        return INVALID_SLOT;

    // Тот же эпохо-гейт, что и для свежего кадра: пере-рендер последнего слота со старой
    // раскладкой на новом дереве дал бы то самое мерцание — лучше короткий hold.
    if (slots_data[lr].epoch != required_epoch_)
        return INVALID_SLOT;

    return lr;
}

// Зеркало MarkRenderingUnsafe для вычислительной стадии: флаг готовности снимает ТОТ,
// КТО СЛОТ ЗАБРАЛ, а не тот, кто закончил. Иначе в окне между выбором и сабмитом слот
// выглядел бы готовым и его мог бы схватить кто-то ещё.
// last_rendering_slot здесь не трогаем: fallback — понятие рендера.
void SlotController::MarkComputingUnsafe(uint8_t slot)
{
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_COMPUTING) & ~SLOT_FLAG_HAS_PREPARED);
}

// Пометка «ушёл на GPU» ставится в момент ВЫБОРА слота (под mutex_), а не после
// сабмита: в окне между выбором и сабмитом sim не должен захватить слот на запись.
// last_rendering_slot — последний отправленный; fallback пользуется им только
// после снятия IS_RENDERING (его снимет FenceThread по завершении fence).
void SlotController::MarkRenderingUnsafe(uint8_t slot)
{
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags | SLOT_FLAG_IS_RENDERING) & ~SLOT_FLAG_HAS_COMPUTED);

    if (last_rendering_slot != slot) {
        last_rendering_slot = slot;
        cv_free_.notify_all();  // старый lr перестал быть fallback-источником — sim может писать
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
        // Fallback'а нет намеренно: пере-вычислять прошлый слот бессмысленно, его
        // результат уже лежит в его же буферах. Нечего считать — просто ждём, как upload.
        cv_computable_.wait(lock);
    }
}

void SlotController::NotifyShutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_.store(true, std::memory_order_release);
    }
    // Будим ВСЕ ожидания: стоящий на condvar поток иначе не увидит выключения running.
    cv_computable_.notify_all();
    cv_renderable_.notify_all();
    cv_free_.notify_all();
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

    cv_computable_.notify_one();  // вычислительной стадии появился кадр (раньше будили рендер)
    cv_free_.notify_all();        // в skip-режиме готовый кадр — цель для перезаписи
}

void SlotController::HandleComputing(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Флаги уже выставил MarkComputingUnsafe при захвате — здесь только фиксация факта
    // сабмита. Отдельный переход нужен, чтобы стадия зеркалила остальные и читалась так же.
    (void)slot;
}

void SlotController::HandleComputed(uint8_t slot)
{
    std::lock_guard<std::mutex> lock(mutex_);

    slots_data[slot].fence = nullptr;
    slots_data[slot].flags = static_cast<uint8_t>(
        (slots_data[slot].flags & ~SLOT_FLAG_IS_COMPUTING) | SLOT_FLAG_HAS_COMPUTED);

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
    case SlotState::UPLOADING: HandleUploading(slot); break;
    case SlotState::PREPARED:  HandlePrepared(slot);  break;
    case SlotState::COMPUTING: HandleComputing(slot); break;
    case SlotState::COMPUTED:  HandleComputed(slot);  break;
    case SlotState::RENDERED:  HandleRendered(slot);  break;
    }
}

void SlotController::SetSlotFence(uint8_t slot, SDL_GPUFence* fence)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].fence = fence;
}

void SlotController::StampSlotEpoch(uint8_t slot, uint64_t epoch)
{
    if (slot == INVALID_SLOT || slot >= BUFFERING_LEVEL)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    slots_data[slot].epoch = epoch;
    // Монотонно двигаем планку: на ребилде epoch > required_epoch_ → старые слоты выпадают
    // из выдачи (и из fallback), рендер держит кадр до готовности этого слота. Пробуждать
    // никого не надо — планка лишь СУЖАЕТ множество отдаваемых кадров; свежий слот разбудит
    // рендер сам через HandlePrepared, когда дозальётся.
    if (epoch > required_epoch_)
        required_epoch_ = epoch;
}

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
