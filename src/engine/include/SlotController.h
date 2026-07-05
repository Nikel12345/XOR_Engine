#pragma once
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "config.h"
#include <Utils.h>

// Жизненный цикл слота (флаги):
//   RESERVED  — sim-поток захватил слот и заливает transfer-буферы (prepare)
//   UPLOADING — copy-команды отправлены на GPU, upload-fence ещё не отстрелил;
//               кадр НЕЛЬЗЯ отдавать рендеру (данные могут не дойти — барьеры
//               copy-пасса на практике этого не гарантируют, нужен fence)
//   PREPARED  — загрузка завершена, кадр готов и ждёт рендера
//   RENDERING — кадр отправлен на GPU, render-fence ещё не отстрелил
//
// «Записываемый» для sim = нет RESERVED/UPLOADING/RENDERING и слот не
// last_rendering_slot (его буферы может читать fallback-пере-рендер). Мешает ли
// записи PREPARED — зависит от режима (allow_frame_skip в Get/WaitFreeSlotIndex):
//   skip разрешён (UPS_priority=true)  — PREPARED перезаписывается, причём всегда
//     САМЫЙ СТАРЫЙ из готовых (наименее ценный кадр): sim никогда не ждёт рендер;
//   skip запрещён (UPS_priority=false) — каждый подготовленный кадр обязан
//     отрисоваться, sim блокируется и UPS проседает до темпа рендера.
//
// PREPARED-кадры образуют логическую очередь по свежести: позиция = frame_id
// (номер prepare, ставится при отправке загрузки), членство = флаг PREPARED.
// Sim перезаписывает старый конец, рендер при skip'е берёт свежий конец (более
// старые кадры при этом умирают — показывать их после нового значит откат),
// при lockstep — старый (по порядку, без потерь). Физическая очередь не нужна:
// концы = argmin/argmax по BUFFERING_LEVEL слотам под уже взятым мьютексом,
// а слоты умеют покидать середину (drop, перезапись) — ring это не выразит.
enum SlotState : uint8_t { UPLOADING, PREPARED, RENDERED };

constexpr uint8_t SLOT_FLAG_RESERVED     = 1u << 0;
constexpr uint8_t SLOT_FLAG_IS_UPLOADING = 1u << 1;
constexpr uint8_t SLOT_FLAG_HAS_PREPARED = 1u << 2;
constexpr uint8_t SLOT_FLAG_IS_RENDERING = 1u << 3;

struct SlotData {
    uint32_t frame_id = 0;              // порядковый номер prepare — порядок sim-тиков, не порядок прихода fences
    uint8_t  flags = 0;                 // защищается mutex_ внутри SlotController
    SDL_GPUFence* fence = nullptr;      // upload-fence при UPLOADING, render-fence при RENDERING (взаимоисключающие)
};

static constexpr uint8_t INVALID_SLOT = 0xFF;

class SlotController {
public:
    SlotController();
    ~SlotController();

    // Sim: захват слота под запись. Резервация происходит АТОМАРНО с выбором
    // (под mutex_): ставится RESERVED (при skip'е заодно снимается PREPARED) —
    // рендер не может взять слот в середине заливки его буферов.
    uint8_t GetFreeSlotIndex(bool allow_frame_skip);    // неблокирующий: INVALID_SLOT, если писать некуда
    uint8_t WaitFreeSlotIndex(bool allow_frame_skip);   // блокирующий

    // Render: PREPARED-слот (latest_wins: свежайший + drop старых / старейший без
    // потерь), иначе fallback на last_rendering_slot (задел на будущее: пере-рендер
    // последнего кадра с per-render данными). Возвращаемый слот помечается
    // IS_RENDERING здесь же, под mutex_ — иначе sim мог бы захватить его в окне
    // между выбором и сабмитом.
    uint8_t WaitRenderableSlot(bool latest_wins);

    bool IsUploadingSlot(uint8_t slot);   // гейт UploadThread
    bool IsRenderingSlot(uint8_t slot);   // гейт FenceThread

    SlotData* GetSlotsData() { return slots_data; }

    void SetSlotState(uint8_t slot, SlotState new_state);
    void SetSlotFence(uint8_t slot, SDL_GPUFence* fence);

    void DebugDump(const char* tag = nullptr);

private:
    SlotData slots_data[BUFFERING_LEVEL];

    // Последний ОТПРАВЛЕННЫЙ на рендер слот (не «завершённый»). Fallback пользуется
    // им только после снятия IS_RENDERING — см. GetRenderableFallbackUnsafe.
    uint8_t last_rendering_slot;

    uint8_t  next_free_slot_index = 0;
    uint32_t prepared_seq = 0;

    std::mutex mutex_;
    std::condition_variable cv_free_;        // sim ждёт записываемый слот
    std::condition_variable cv_renderable_;  // render ждёт готовый кадр / fallback

    // Все *Unsafe — только под уже захваченным mutex_.
    uint8_t AcquireFreeSlotUnsafe(bool allow_frame_skip);
    uint8_t GetReadySlotUnsafe(bool latest_wins);
    uint8_t GetRenderableFallbackUnsafe();
    void    MarkRenderingUnsafe(uint8_t slot);

    void HandleUploading(uint8_t slot);
    void HandlePrepared(uint8_t slot);
    void HandleRendered(uint8_t slot);
};
