#include "PCH.h"
#include "ThreadController.h"
#include "SlotController.h"
#include "EngineProfiler.h"

static constexpr bool UPS_priority = false;
static constexpr bool FPS_NoLimit = false;
static constexpr bool UPS_NoLimit = false;

ThreadController::ThreadController(SlotController* slot_controller)
{
    this->slot_controller = slot_controller;
    fps_counter = new AvgRateCounter("FPS", 20);
    ups_counter = new AvgRateCounter("UPS", 20);
}

void ThreadController::SetComputeCallback(ComputeCallback cb)
{
    compute_callback = std::move(cb);
}

void ThreadController::SetGameIterationCallback(GameIterCallback cb)
{
    game_iter_callback = std::move(cb);
}

void ThreadController::SetPrepareCallback(PrepareCallback cb)
{
    prepare_callback = std::move(cb);
}

void ThreadController::SetUploadCallback(UploadCallback cb)
{
    upload_callback = std::move(cb);
}

void ThreadController::SetRenderCallback(RenderCallback cb)
{
    render_callback = std::move(cb);
}

void ThreadController::SetFenceCallback(FenceCallback cb)
{
    fence_callback = std::move(cb);
}

void ThreadController::StartThreads()
{
    if (!game_iter_callback || !prepare_callback || !upload_callback || !compute_callback || !render_callback || !fence_callback) {
        if (!game_iter_callback) {
            SDL_Log("No game_iter");
        }
        if (!prepare_callback) {
            SDL_Log("No prep");
        }
        if (!upload_callback) {
            SDL_Log("No upload");
        }
        if (!compute_callback) {
            SDL_Log("No compute");
        }
        if (!render_callback) {
            SDL_Log("No render");
        }
        if (!fence_callback) {
            SDL_Log("No fence");
        }
        return;
    }
    running.store(true);
    game_n_prep_iter_thread = std::thread(&ThreadController::SimulationThread, this);

    // ДИАГНОСТИКА (config.h): часть конвейера не поднимается. Sim при этом не встаёт —
    // незанятые слоты переиспользуются.
    if (!DISABLE_UPLOAD)
        upload_thread = std::thread(&ThreadController::UploadThread, this);
    compute_thread = std::thread(&ThreadController::ComputeThread, this);
    if (!DISABLE_RENDER) {
        render_thread = std::thread(&ThreadController::RenderThread, this);
        fence_thread  = std::thread(&ThreadController::FenceThread, this);
    }
    if (DISABLE_RENDER || DISABLE_UPLOAD)
        SDL_Log("ThreadController: DIAG mode - DISABLE_RENDER=%d DISABLE_UPLOAD=%d",
                (int)DISABLE_RENDER, (int)DISABLE_UPLOAD);
}

ThreadController::~ThreadController()
{
    Shutdown();
}

void ThreadController::Shutdown()
{
    running.store(false);
    // ДО join'ов: поток на condvar слота сам по себе выключения running не увидит.
    if (slot_controller)
        slot_controller->NotifyShutdown();
    if (game_n_prep_iter_thread.joinable())
        game_n_prep_iter_thread.join();
    if (upload_thread.joinable())
        upload_thread.join();
    if (compute_thread.joinable())
        compute_thread.join();
    if (render_thread.joinable())
        render_thread.join();
    if (fence_thread.joinable())
        fence_thread.join();
}

const double TARGET_UPS = 1000.0 / 60.0;
const double TARGET_FPS = 1000.0 / 60.0;

void ThreadController::SimulationThread()
{
    while (running.load())
    {
        auto frame_start = std::chrono::high_resolution_clock::now();

        ups_counter->start();

        // slot_wait: большое время здесь = sim голодает по слотам, то есть узкое место
        // в РЕНДЕРЕ, а не в подготовке кадра.
        uint8_t slot;
        {
            PROF_SCOPE(Sim, "slot_wait (ожидание свободного слота)");
            slot = slot_controller->GetFreeSlotIndex(UPS_priority);
            if (!UPS_priority and slot == INVALID_SLOT)
            {
                slot = slot_controller->WaitFreeSlotIndex(UPS_priority);
            }
        }
        // Из ожидания могли выпустить остановкой, а не свободным слотом: выходим ДО
        // игрового колбэка.
        if (!running.load(std::memory_order_relaxed))
            break;

        {
            PROF_SCOPE(Sim, "game_iter (Game::MainIterate)");
            game_iter_callback();
        }
        if (slot != INVALID_SLOT) {
            PROF_SCOPE(Sim, "prepare_total (Engine::PrepareFunc)");
            prepare_callback(slot);
        }
        ups_counter->end();
        PROF_FRAME(Sim);

        if (UPS_NoLimit)
        {
            continue;
        }
        auto frame_end = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();

        if (elapsed_ms < TARGET_UPS)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds((int)(TARGET_UPS - elapsed_ms))
            );
        }
    }
}


void ThreadController::UploadThread()
{
    while (running.load(std::memory_order_relaxed))
    {
        bool processed = false;

        for (uint8_t slot = 0; slot < BUFFERING_LEVEL; ++slot)
        {
            if (!slot_controller->IsUploadingSlot(slot)) {
                continue;
            }
            // Фенсы ставятся ДО перевода в UPLOADING — проверка защитная.
            if (slot_controller->GetSlotsData()[slot].upload.Empty()) {
                continue;
            }

            upload_callback(slot);
            processed = true;
        }

        if (!processed)
        {
            // Пока конвейер полон, цикл сюда не попадает: стадия стоит в kernel-wait.
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
}

void ThreadController::ComputeThread()
{
    while (running.load())
    {
        uint8_t slot = slot_controller->WaitComputableSlot(UPS_priority);
        if (slot == INVALID_SLOT)   // останов
            break;

        compute_callback(slot);
    }
}

void ThreadController::RenderThread()
{
    if (!render_callback) {
        SDL_Log("ThreadController::RenderThread: no render callback set");
        return;
    }

    while (running.load())
    {
        auto frame_start = std::chrono::high_resolution_clock::now();

        uint8_t slot = slot_controller->WaitRenderableSlot(UPS_priority);
        if (slot == INVALID_SLOT)   // останов
            break;
        fps_counter->start();

        while (running.load())
        {
            if (render_callback(slot))
            {
                fps_counter->end();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        if (FPS_NoLimit)
        {
            continue;
        }
        auto frame_end = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(
            frame_end - frame_start
        ).count();

        if (elapsed_ms < TARGET_FPS)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds((int)(TARGET_FPS - elapsed_ms))
            );
        }
    }
}

void ThreadController::FenceThread()
{
    while (running.load(std::memory_order_relaxed))
    {
        bool processed = false;

        for (uint8_t slot = 0; slot < BUFFERING_LEVEL; ++slot)
        {
            if (!slot_controller->IsRenderingSlot(slot)) {
                continue;
            }
            // IS_RENDERING ставится при ВЫБОРЕ слота, фенс появляется после сабмита.
            if (slot_controller->GetSlotsData()[slot].render.Empty()) {
                continue;
            }

            fence_callback(slot);
            processed = true;
        }

        if (!processed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
