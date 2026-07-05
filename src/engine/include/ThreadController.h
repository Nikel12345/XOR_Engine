#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include "FPSCounter.h"

class SlotController;

class ThreadController {
public:
    using GameIterCallback = std::function<void()>;
    using PrepareCallback = std::function<void(uint8_t slot)>;
    using UploadCallback = std::function<void(uint8_t slot)>;
    using RenderCallback = std::function<bool(uint8_t slot)>;
    using FenceCallback = std::function<void(uint8_t slot)>;

    explicit ThreadController(SlotController* slot_controller);
    ~ThreadController();

    ThreadController(const ThreadController&) = delete;
    ThreadController& operator=(const ThreadController&) = delete;

    void SetGameIterationCallback(GameIterCallback cb);
    void SetPrepareCallback(PrepareCallback cb);
    void SetUploadCallback(UploadCallback cb);
    void SetRenderCallback(RenderCallback cb);
    void SetFenceCallback(FenceCallback cb);

    void StartThreads();
    AvgRateCounter* fps_counter = nullptr;
    AvgRateCounter* ups_counter = nullptr;
private:
    void SimulationThread();
    void UploadThread();
    void RenderThread();
    void FenceThread();

    std::atomic<bool> running{ false };
    SlotController* slot_controller = nullptr;

    GameIterCallback game_iter_callback;
    PrepareCallback prepare_callback;
    UploadCallback upload_callback;
    RenderCallback render_callback;
    FenceCallback fence_callback;

    std::thread game_n_prep_iter_thread;
    std::thread upload_thread;
    std::thread render_thread;
    std::thread fence_thread;


};
