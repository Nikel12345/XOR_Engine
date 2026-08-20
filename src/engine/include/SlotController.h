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
//   PREPARED  — загрузка завершена, кадр ждёт ВЫЧИСЛИТЕЛЬНОЙ стадии
//   COMPUTING — compute-команды отправлены, compute-fence ещё не отстрелил
//   COMPUTED  — вычисления завершены, кадр готов и ждёт рендера
//   RENDERING — кадр отправлен на GPU, render-fence ещё не отстрелил
//
// «Записываемый» для sim = нет RESERVED/UPLOADING/COMPUTING/RENDERING и слот не
// last_rendering_slot (его буферы может читать fallback-пере-рендер). Мешает ли
// записи PREPARED — зависит от режима (allow_frame_skip в Get/WaitFreeSlotIndex):
//   skip разрешён (UPS_priority=true)  — PREPARED перезаписывается, причём всегда
//     САМЫЙ СТАРЫЙ из готовых (наименее ценный кадр): sim никогда не ждёт рендер;
//   skip запрещён (UPS_priority=false) — каждый подготовленный кадр обязан
//     отрисоваться, sim блокируется и UPS проседает до темпа рендера.
//
// УСТАРИВАНИЕ ЖИВЁТ В ОДНОМ МЕСТЕ. Снимать флаг готовности с ЧУЖИХ слотов (skip
// «показать не успели») имеет право только вычислительная стадия: она берёт
// свежайший PREPARED и гасит более старые. Рендер выбирает среди COMPUTED по
// frame_id и чужих слотов НЕ трогает — до него доезжает уже отфильтрованное.
// Раньше это делал рендер, и разрушительная часть жила в двух местах сразу.
//
// PREPARED/COMPUTED-кадры образуют логическую очередь по свежести: позиция = frame_id
// (номер prepare, ставится при отправке загрузки), членство = флаг PREPARED.
// Sim перезаписывает старый конец, рендер при skip'е берёт свежий конец (более
// старые кадры при этом умирают — показывать их после нового значит откат),
// при lockstep — старый (по порядку, без потерь). Физическая очередь не нужна:
// концы = argmin/argmax по BUFFERING_LEVEL слотам под уже взятым мьютексом,
// а слоты умеют покидать середину (drop, перезапись) — ring это не выразит.
enum class SlotState : uint8_t { UPLOADING, PREPARED, COMPUTING, COMPUTED, RENDERED };   // scoped: имена слишком общие для глобала

constexpr uint8_t SLOT_FLAG_RESERVED     = 1u << 0;
constexpr uint8_t SLOT_FLAG_IS_UPLOADING = 1u << 1;
constexpr uint8_t SLOT_FLAG_HAS_PREPARED = 1u << 2;
constexpr uint8_t SLOT_FLAG_IS_COMPUTING = 1u << 3;
constexpr uint8_t SLOT_FLAG_HAS_COMPUTED = 1u << 4;
constexpr uint8_t SLOT_FLAG_IS_RENDERING = 1u << 5;

// Fences ОДНОЙ стадии слота — то есть всех сабмитов, которые эта стадия сделала для этого
// слота. Поле такое есть ровно у тех стадий, чей fence отрабатывает ДРУГОЙ поток: prepare
// только ставит задачи (ждёт UploadThread), render только пишет команды (ждёт FenceThread).
// У compute такого поля нет и не должно быть: он не разводит вход и выход по потокам, его
// работа обязана завершиться внутри его же вызова — fence он ждёт на месте, локальным.
//
// Больше одного fence бывает, когда стадия пишет в РАЗНЫЕ очереди (заливка: буферы на
// копировальную, текстуры на графическую): межочередного порядка нет, поэтому ждутся они
// вместе, одним wait_all. (items, count) — ровно та форма, что принимает SDL_WaitForGPUFences.
//
// Вектор не нужен: верхняя граница — «в сколько очередей пишет одна стадия», и молча она не
// вырастет. Фиксированный массив заодно убирает аллокацию с кадрового пути.
struct StageFences {
    static constexpr uint8_t CAP = 2;
    SDL_GPUFence* items[CAP] = {};
    uint8_t       count = 0;
    // [PROFILE] Момент сабмита стадии. Разница now() − submit_time при срабатывании fence =
    // реальная GPU-латентность стадии. Лежит ЗДЕСЬ, а не в SlotData, по той же причине, что и
    // сами fences: у каждой стадии свой, общее поле молча означало бы «чей сейчас — угадай».
    std::chrono::steady_clock::time_point submit_time{};

    void Push(SDL_GPUFence* f) { if (f && count < CAP) items[count++] = f; }
    bool Empty() const { return count == 0; }
    void Clear() { count = 0; }
};

struct SlotData {
    uint64_t frame_id = 0;              // порядковый номер prepare — порядок sim-тиков, не порядок прихода fences
    // Эпоха ПОЛНОГО ребилда дерева батчей, под которой готовились буферы слота. Рендер не
    // берёт слот (и не откатывается на fallback), если его epoch != required_epoch_ — так после
    // редкой пересборки дерева не показывается кадр со старой раскладкой indirect/out_pib.
    uint64_t epoch = 0;
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

    // Вычислительная стадия. Fallback'а у неё НЕТ (в отличие от рендера): нечего считать —
    // просто ждём, как upload. Пере-«вычислять» прошлый слот бессмысленно, его результат уже
    // лежит в его же буферах.
    uint8_t WaitComputableSlot(bool latest_wins);

    uint8_t WaitRenderableSlot(bool latest_wins);

    bool IsUploadingSlot(uint8_t slot);   // гейт UploadThread
    bool IsRenderingSlot(uint8_t slot);   // гейт FenceThread

    SlotData* GetSlotsData() { return slots_data; }

    void SetSlotState(uint8_t slot, SlotState new_state);
    // Публикация fence'ов стадии — обе ДО перевода слота в соответствующее состояние: поток,
    // который будет их ждать, гейтится флагом, и увидеть флаг раньше fence он не должен.
    // Заливка добавляет по одному на очередь (Push, до CAP), рендер сабмитит ровно один.
    void PushUploadFence(uint8_t slot, SDL_GPUFence* fence);
    void SetRenderFence(uint8_t slot, SDL_GPUFence* fence);

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

    // Разбудить всех ждущих и перевести контроллер в режим останова: ожидания перестают
    // блокировать и возвращают INVALID_SLOT. Без этого поток, стоящий на condvar, не проснётся
    // при выключении running — и join повиснет. Зовёт ~ThreadController ДО join'ов.
    void NotifyShutdown();
    bool IsShuttingDown() const { return shutting_down_.load(std::memory_order_acquire); }

    // Чем рендер занял свои кадры: свежим COMPUTED (сразу или после SOFT_WAIT) или пере-рендером
    // last_rendering_slot. Разница FPS и UPS — это РОВНО fallback: рендеру новый кадр не нужен,
    // он переиспользует слот, а sim в это же время может голодать по слотам (см. WARNINGS.md,
    // «Пустой сабмит на графическую очередь»). Из-за этого «FPS выше UPS» само по себе ничего не
    // доказывает — доказывает вот эта тройка.
    //
    // fresh_after_wait отдельно от fresh намеренно: он показывает, окупается ли SOFT_WAIT — то
    // есть как часто двухмиллисекундное ожидание реально спасло кадр от дубля.
    //
    // Здесь ТОЛЬКО счётчики. Контроллер их не печатает и никуда не отдаёт: вывод — дело того,
    // кто спросил (профайлер, оверлей редактора). Счётчики монотонные, за окно считать разностью.
    struct RenderChoiceStats { uint64_t fresh = 0, fresh_after_wait = 0, fallback = 0; };
    RenderChoiceStats GetRenderChoiceStats() const;

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

    // Пишутся под mutex_ (все три точки выбора — в WaitRenderableSlot), читаются откуда угодно:
    // атомик нужен ради чтения, не ради инкремента. relaxed — счётчик ничего не упорядочивает.
    std::atomic<uint64_t> stat_fresh_{ 0 };
    std::atomic<uint64_t> stat_fresh_after_wait_{ 0 };
    std::atomic<uint64_t> stat_fallback_{ 0 };
    std::atomic<bool>     shutting_down_{ false };    // см. NotifyShutdown

    std::mutex mutex_;
    std::condition_variable cv_free_;        // sim ждёт записываемый слот
    std::condition_variable cv_computable_;  // compute ждёт залитый кадр
    std::condition_variable cv_renderable_;  // render ждёт готовый кадр / fallback

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
