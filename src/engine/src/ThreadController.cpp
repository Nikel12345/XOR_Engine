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

    // ДИАГНОСТИКА (config.h): по флагам не поднимаем часть конвейера, чтобы замерить
    // store без контеншена рендера/аплоада. Sim при этом не встаёт — frame skip
    // переиспользует слоты (upload сам прокручивает слот в PrepareFuncPrepassUndepended).
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
    // ДО join'ов: поток, стоящий на condvar слота, сам по себе выключения running не увидит.
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

        // UPS_priority=true: sim первичен — готовые, но не отрисованные кадры
        // перезаписываются (frame skip), sim никогда не ждёт рендер.
        // UPS_priority=false: рендер первичен — каждый подготовленный кадр обязан
        // быть показан, sim блокируется и UPS проседает до темпа рендера.
        // slot_wait: если тут большое время — sim голодает по свободным слотам,
        // то есть узкое место в РЕНДЕРЕ, а не в подготовке кадра.
        uint8_t slot;
        {
            PROF_SCOPE(Sim, "slot_wait (ожидание свободного слота)");
            slot = slot_controller->GetFreeSlotIndex(UPS_priority);
            if (!UPS_priority and slot == INVALID_SLOT)
            {
                slot = slot_controller->WaitFreeSlotIndex(UPS_priority);
            }
        }
        // Из ожидания могли выпустить остановкой, а не свободным слотом. Выходим ДО игрового
        // колбэка: гонять ещё один тик игры на сносе конвейера незачем.
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
            // Fences ставятся ДО перевода в UPLOADING, но проверка защитная.
            if (slot_controller->GetSlotsData()[slot].upload.Empty()) {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
}

// Вычислительная стадия между загрузкой и рендером. Устройство ближе к рендеру, чем к
// upload: сама берёт слот (WaitComputableSlot) и сама пишет команды, тогда как UploadThread
// лишь сторожит чужие сабмиты. Отличие от рендера — нет fallback'а: пере-вычислять прошлый
// слот бессмысленно, его результат уже лежит в его же буферах.
//
// Завершение работы GPU ловится fence'ом внутри колбэка, как и на других стадиях; сюда он
// возвращается уже с готовым слотом и сам переводит его в COMPUTED. Колбэк ОБЯЗАТЕЛЕН, как и
// у остальных стадий: пустая стадия — это пустая функция, а не отсутствие стадии.
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
        if (slot == INVALID_SLOT)   // останов (как в ComputeThread)
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
            // IS_RENDERING ставится при ВЫБОРЕ слота (WaitRenderableSlot), fence
            // появляется позже — после сабмита. Пока его нет, ждать нечего.
            if (slot_controller->GetSlotsData()[slot].render.Empty()) {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
