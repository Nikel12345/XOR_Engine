#pragma once
#include <cstdint>
#include <atomic>
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
enum class SlotState : uint8_t { UPLOADING, PREPARED, RENDERED };   // scoped: имена слишком общие для глобала

constexpr uint8_t SLOT_FLAG_RESERVED     = 1u << 0;
constexpr uint8_t SLOT_FLAG_IS_UPLOADING = 1u << 1;
constexpr uint8_t SLOT_FLAG_HAS_PREPARED = 1u << 2;
constexpr uint8_t SLOT_FLAG_IS_RENDERING = 1u << 3;

struct SlotData {
    uint64_t frame_id = 0;              // порядковый номер prepare — порядок sim-тиков, не порядок прихода fences
    // Эпоха ПОЛНОГО ребилда дерева батчей, под которой готовились буферы слота. Рендер не
    // берёт слот (и не откатывается на fallback), если его epoch != required_epoch_ — так после
    // редкой пересборки дерева не показывается кадр со старой раскладкой indirect/out_pib.
    uint64_t epoch = 0;
    uint8_t  flags = 0;                 // защищается mutex_ внутри SlotController
    SDL_GPUFence* fence = nullptr;      // upload-fence при UPLOADING, render-fence при RENDERING (взаимоисключающие)
    // [PROFILE] Момент сабмита командбуфера, чей fence сейчас в поле fence — ОДНО поле
    // на обе стадии по той же причине, что и сам fence: слот идёт UPLOADING → PREPARED →
    // RENDERING, поэтому upload- и render-сабмит не пересекаются во времени.
    //   • пишется в PrepareFuncPrepassUndepended (upload) и в RenderFunc (render), до публикации fence;
    //   • читается в UploadFunc и в FenceFunc сразу после срабатывания fence.
    // Разница now() − submit_time = реальная GPU-латентность стадии (submit → сигнал fence).
    std::chrono::steady_clock::time_point submit_time{};
};

static constexpr uint8_t INVALID_SLOT = 0xFF;

class SlotController {
public:
    SlotController();
    ~SlotController();

    uint8_t GetFreeSlotIndex(bool allow_frame_skip); 
    uint8_t WaitFreeSlotIndex(bool allow_frame_skip);

    uint8_t WaitRenderableSlot(bool latest_wins);

    bool IsUploadingSlot(uint8_t slot);   // гейт UploadThread
    bool IsRenderingSlot(uint8_t slot);   // гейт FenceThread

    SlotData* GetSlotsData() { return slots_data; }

    void SetSlotState(uint8_t slot, SlotState new_state);
    void SetSlotFence(uint8_t slot, SDL_GPUFence* fence);

    // Клеймит слоту эпоху ребилда, под которой залиты его буферы, и поднимает планку
    // required_epoch_ до неё. Зовётся каждый prepare (на не-ребилд-кадрах эпоха та же —
    // инертно). На полном ребилде эпоха скакнула → рендер держит кадр, пока перестроенный
    // слот не дозальётся, вместо мерцания старой раскладкой.
    void StampSlotEpoch(uint8_t slot, uint64_t epoch);

    // Текущая планка эпохи (см. required_epoch_). Читает sim при дренаже трэша пайплайнов
    // (PrepareFunc → TrashPipelines): released только после того, как планка ушла дальше эпохи
    // инвалидации (рендер уже не покажет слоты со слепками, держащими старый указатель).
    uint64_t RequiredEpoch() { std::lock_guard<std::mutex> lk(mutex_); return required_epoch_; }

    // Счётчик завершённых render-fence. Тикает FenceThread — единственное, что он сообщает о
    // жизни GPU-кадров; ФАКТИЧЕСКИЕ release отложенных ресурсов делает sim в prepare, сравнивая
    // стампы записей трэшей с этим счётчиком (очереди остаются одно-поточными, без замков).
    void NotifyRenderFenceDone() { render_fences_done_.fetch_add(1, std::memory_order_release); }
    uint64_t RenderFencesDone() const { return render_fences_done_.load(std::memory_order_acquire); }

    void DebugDump(const char* tag = nullptr);

private:
    SlotData slots_data[BUFFERING_LEVEL];

    // Последний ОТПРАВЛЕННЫЙ на рендер слот (не «завершённый»). Fallback пользуется
    // им только после снятия IS_RENDERING — см. GetRenderableFallbackUnsafe.
    uint8_t last_rendering_slot;

    uint8_t  next_free_slot_index = 0;
    uint64_t prepared_seq = 0;

    // Планка эпохи: рендер показывает только слоты с epoch == required_epoch_. Поднимается
    // в StampSlotEpoch (монотонно, вслед за rebuild_epoch дерева). Инертна, пока нет ребилда.
    uint64_t required_epoch_ = 0;

    std::atomic<uint64_t> render_fences_done_{ 0 };   // см. NotifyRenderFenceDone

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
