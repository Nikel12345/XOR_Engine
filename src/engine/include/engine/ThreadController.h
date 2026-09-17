#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include "FPSCounter.h"

class SlotController;

// ── ДИАГНОСТИКА контеншена store (разбор 2026-07-05): отключают фазы конвейера, чтобы
//    замерить время store в профайлере без нагрузки рендера/аплоада на память и PCIe.
//    В UPS_priority sim продолжает готовить кадры (frame skip переиспользует неотрисованные
//    слоты), так что `DefaultTransformBuffer .store` меряется непрерывно. В норме ОБА false.
//      DISABLE_RENDER — не стартовать render+fence потоки (нет рендер-GPU-работы);
//      DISABLE_UPLOAD — не стартовать upload; PrepareFunc гоняет store БЕЗ submit на GPU
//                       (нет upload-DMA), слот прокручивается вручную для frame skip.
static constexpr bool DISABLE_RENDER = false;
static constexpr bool DISABLE_UPLOAD = false;

class ThreadController {
public:
    using GameIterCallback = std::function<void()>;
    using PrepareCallback = std::function<void(uint8_t slot)>;
    using UploadCallback = std::function<void(uint8_t slot)>;
    using ComputeCallback = std::function<void(uint8_t slot)>;
    using RenderCallback = std::function<bool(uint8_t slot)>;
    using FenceCallback = std::function<void(uint8_t slot)>;

    explicit ThreadController(SlotController* slot_controller);
    ~ThreadController();

    ThreadController(const ThreadController&) = delete;
    ThreadController& operator=(const ThreadController&) = delete;

    void SetGameIterationCallback(GameIterCallback cb);
    void SetPrepareCallback(PrepareCallback cb);
    void SetUploadCallback(UploadCallback cb);
    void SetComputeCallback(ComputeCallback cb);
    void SetRenderCallback(RenderCallback cb);
    void SetFenceCallback(FenceCallback cb);

    void StartThreads();
    // Идемпотентен, поэтому зовётся и явно, и из dtor. Явный вызов обязателен там, где после
    // цикла разрушается что-то, что держит sim-поток: игровой колбэк замкнут на объект игры.
    void Shutdown();
    AvgRateCounter* fps_counter = nullptr;
    AvgRateCounter* ups_counter = nullptr;
private:
    void SimulationThread();
    void UploadThread();
    void ComputeThread();
    void RenderThread();
    void FenceThread();

    std::atomic<bool> running{ false };
    SlotController* slot_controller = nullptr;

    GameIterCallback game_iter_callback;
    PrepareCallback prepare_callback;
    UploadCallback upload_callback;
    ComputeCallback compute_callback;
    RenderCallback render_callback;
    FenceCallback fence_callback;

    std::thread game_n_prep_iter_thread;
    std::thread upload_thread;
    std::thread compute_thread;
    std::thread render_thread;
    std::thread fence_thread;


};
