#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "config.h"
#include <Utils.h>

// Состояния слота, дисциплина выбора кадра и эпоха ребилда — docs/internals/frame.md.
enum class SlotState : uint8_t { UPLOADING, PREPARED, COMPUTING, COMPUTED, RENDERED };

constexpr uint8_t SLOT_FLAG_RESERVED     = 1u << 0;
constexpr uint8_t SLOT_FLAG_IS_UPLOADING = 1u << 1;
constexpr uint8_t SLOT_FLAG_HAS_PREPARED = 1u << 2;
constexpr uint8_t SLOT_FLAG_IS_COMPUTING = 1u << 3;
constexpr uint8_t SLOT_FLAG_HAS_COMPUTED = 1u << 4;
constexpr uint8_t SLOT_FLAG_IS_RENDERING = 1u << 5;

// Фенсы одной стадии слота: по одному на каждую очередь, куда она сабмитила.
// (items, count) — форма аргументов SDL_WaitForGPUFences.
struct StageFences {
    static constexpr uint8_t CAP = 2;
    SDL_GPUFence* items[CAP] = {};
    uint8_t       count = 0;
    // [PROFILE] now() − submit_time на срабатывании фенса = GPU-латентность стадии.
    std::chrono::steady_clock::time_point submit_time{};

    void Push(SDL_GPUFence* f) { if (f && count < CAP) items[count++] = f; }
    bool Empty() const { return count == 0; }
    void Clear() { count = 0; }
};

struct SlotData {
    uint64_t frame_id = 0;              // порядок sim-тиков, не порядок прихода фенсов
    uint64_t epoch = 0;                 // эпоха ребилда дерева, под которой залиты буферы слота
    uint8_t  flags = 0;                 // защищается mutex_ внутри SlotController
    StageFences upload;                 // сабмитит sim (prepare), ждёт UploadThread
    StageFences render;                 // сабмитит render-поток,  ждёт FenceThread
};

static constexpr uint8_t INVALID_SLOT = 0xFF;

class SlotController {
public:
    SlotController();
    ~SlotController();

    uint8_t GetFreeSlotIndex(bool allow_frame_skip); 
    uint8_t WaitFreeSlotIndex(bool allow_frame_skip);

    uint8_t WaitComputableSlot(bool latest_wins);

    uint8_t WaitRenderableSlot(bool latest_wins);

    bool IsUploadingSlot(uint8_t slot);
    bool IsRenderingSlot(uint8_t slot);

    SlotData* GetSlotsData() { return slots_data; }

    void SetSlotState(uint8_t slot, SlotState new_state);
    // Обе зовутся ДО перевода слота в состояние, которое сторожит другой поток: увидеть флаг
    // раньше фенса он не должен.
    void PushUploadFence(uint8_t slot, SDL_GPUFence* fence);
    void SetRenderFence(uint8_t slot, SDL_GPUFence* fence);

    void StampSlotEpoch(uint8_t slot, uint64_t epoch);

    // Счётчик вместо замков: очереди трэша остаются однопоточными.
    void NotifyRenderFenceDone() { render_fences_done_.fetch_add(1, std::memory_order_release); }
    uint64_t RenderFencesDone() const { return render_fences_done_.load(std::memory_order_acquire); }

    // Зовётся ДО join'ов: иначе поток на condvar не проснётся и join повиснет.
    void NotifyShutdown();
    bool IsShuttingDown() const { return shutting_down_.load(std::memory_order_acquire); }

    struct RenderChoiceStats { uint64_t fresh = 0, fresh_after_wait = 0, fallback = 0; };
    RenderChoiceStats GetRenderChoiceStats() const;

    void DebugDump(const char* tag = nullptr);

private:
    SlotData slots_data[BUFFERING_LEVEL];

    uint8_t last_rendering_slot;

    uint8_t  next_free_slot_index = 0;
    uint64_t prepared_seq = 0;

    uint64_t required_epoch_ = 0;   // отдаются только слоты с epoch == required_epoch_

    std::atomic<uint64_t> render_fences_done_{ 0 };

    std::atomic<uint64_t> stat_fresh_{ 0 };
    std::atomic<uint64_t> stat_fresh_after_wait_{ 0 };
    std::atomic<uint64_t> stat_fallback_{ 0 };
    std::atomic<bool>     shutting_down_{ false };

    std::mutex mutex_;
    std::condition_variable cv_free_;
    std::condition_variable cv_computable_;
    std::condition_variable cv_renderable_;

    // Все *Unsafe — только под уже захваченным mutex_.
    uint8_t AcquireFreeSlotUnsafe(bool allow_frame_skip);
    uint8_t GetComputableSlotUnsafe(bool latest_wins);
    uint8_t GetReadySlotUnsafe(bool latest_wins);
    uint8_t GetRenderableFallbackUnsafe();
    void    MarkComputingUnsafe(uint8_t slot);
    void    MarkRenderingUnsafe(uint8_t slot);

    void HandleUploading(uint8_t slot);
    void HandlePrepared(uint8_t slot);
    void HandleComputing(uint8_t slot);
    void HandleComputed(uint8_t slot);
    void HandleRendered(uint8_t slot);
};
