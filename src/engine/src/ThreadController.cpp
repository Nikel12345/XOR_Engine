#include "PCH.h"
#include "ThreadController.h"
#include "SlotController.h"

static constexpr bool UPS_priority = false;
static constexpr bool FPS_NoLimit = false;
static constexpr bool UPS_NoLimit = false;

ThreadController::ThreadController(SlotController* slot_controller)
{
    this->slot_controller = slot_controller;
    fps_counter = new AvgRateCounter("FPS", 20);
    ups_counter = new AvgRateCounter("UPS", 20);
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
    if (!game_iter_callback || !prepare_callback || !upload_callback || !render_callback || !fence_callback) {
        if (!game_iter_callback) {
            SDL_Log("No game_iter");
        }
        if (!prepare_callback) {
            SDL_Log("No prep");
        }
        if (!upload_callback) {
            SDL_Log("No upload");
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
    upload_thread = std::thread(&ThreadController::UploadThread, this);
    render_thread = std::thread(&ThreadController::RenderThread, this);
    fence_thread = std::thread(&ThreadController::FenceThread, this);
}

ThreadController::~ThreadController()
{
    running.store(false);
    if (game_n_prep_iter_thread.joinable())
        game_n_prep_iter_thread.join();
    if (upload_thread.joinable())
        upload_thread.join();
    if (render_thread.joinable())
        render_thread.join();
    if (fence_thread.joinable())
        fence_thread.join();
}

//const double TARGET_UPS = 60.0;
//const double TARGET_FPS = 10.0;
const double TARGET_UPS = 1000.0 / 60.0;
const double TARGET_FPS = 1000.0 / 60.0;

void ThreadController::SimulationThread()
{
    while (running.load())
    {
        auto frame_start = std::chrono::high_resolution_clock::now();

        ups_counter->start();

        // UPS_priority=true: sim первичен — готовые, но не отрисованные кадры
        // перезаписываются (frame skip), sim никогда не ждёт рендер.
        // UPS_priority=false: рендер первичен — каждый подготовленный кадр обязан
        // быть показан, sim блокируется и UPS проседает до темпа рендера.
        uint8_t slot = slot_controller->GetFreeSlotIndex(UPS_priority);

        if (!UPS_priority and slot == INVALID_SLOT)
        {
            slot = slot_controller->WaitFreeSlotIndex(UPS_priority);

        }

        game_iter_callback();
        if (slot != INVALID_SLOT) {
            prepare_callback(slot);
        }
        ups_counter->end();

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


// Зеркало FenceThread для upload-fences: наблюдает за слотами UPLOADING и по
// завершении загрузки промоутит их в PREPARED (это делает upload_callback).
// Sim при этом свободен готовить следующий слот — латентность сабмита спрятана.
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
            // Fence ставится ДО перевода в UPLOADING, но проверка защитная.
            if (!slot_controller->GetSlotsData()[slot].fence) {
                continue;
            }

            // upload_callback(slot) блокирующе ждёт upload-fence (kernel-wait),
            // возвращает transfer-буферы в пул и зовёт SetSlotState(slot, PREPARED)
            upload_callback(slot);
            processed = true;
        }

        if (!processed)
        {
            // Загрузок в полёте нет — опрашивать чаще нет смысла. Пока пайплайн
            // полон, цикл сюда не попадает (стоит в kernel-wait, не спит).
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
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


        // Режим определяет конец очереди готовых кадров: при UPS_priority (skip)
        // берём свежайший (старые умирают), при lockstep — по порядку без потерь.
        uint8_t slot = slot_controller->WaitRenderableSlot(UPS_priority);
        //slot_controller->DebugDump("RenderThread idle");
        fps_counter->start();
        //auto* slots = slot_controller->GetSlotsData();
        //SDL_Log("RenderThread: slot=%u, flags=0x%02X, frame_id=%u",
        //    slot, slots[slot].flags, slots[slot].frame_id);

        while (running.load())
        {
            if (render_callback(slot))
            {
                fps_counter->end();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
            // IS_RENDERING ставится при ВЫБОРЕ слота (WaitRenderableSlot), fence
            // появляется позже — после сабмита. Пока его нет, ждать нечего.
            if (!slot_controller->GetSlotsData()[slot].fence) {
                continue;
            }

            // fence_callback(slot) блокирующе ждёт fence (kernel-wait, без опроса)
            // и по готовности вызывает slot_controller->SetSlotState(slot, RENDERED)
            fence_callback(slot);
            processed = true;
        }

        if (!processed)
        {
            // Ждать нечего (никто не рендерится или кадр ещё не сабмитнут) —
            // опрашивать чаще нет смысла.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}
