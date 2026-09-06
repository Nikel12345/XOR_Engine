#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

//  EngineProfiler — лёгкий КАДРОВЫЙ профайлер для дебага просадок.
//
//  Каждый именованный слот копит сумму/макс времени (мс) и число вызовов за окно,
//  плюс опциональный «размер» полезной нагрузки (байты) — для буферов. Раз в
//  report_period_ms реального времени Frame() печатает усреднённую сводку и обнуляет
//  окно. Интервал по ВРЕМЕНИ (а не по кадрам): SIM с низким UPS и RENDER с высоким
//  FPS печатаются одинаково регулярно.
//
//  Замер: auto t = Prof::Clock::now(); <работа>; Prof::Sim().Add("name", Prof::MsSince(t));
//  Либо RAII на весь блок:  { PROF_SCOPE(Sim, "name"); <работа> }
//  Конец кадра секции: Prof::Sim().Frame();  (печать раз в ~report_period_ms мс)
//
//  Потокобезопасен: sim- и render-потоки пишут в РАЗНЫЕ экземпляры (Prof::Sim /
//  Prof::Render), а Add()/Frame() всё равно под мьютексом — накладные копейки
//  на фоне замеряемых миллисекунд.
//
//  ENGINE_PROFILE (флаг сборки, задаётся в engine/CMakeLists.txt, дефолт 1):
//    1 — профайлер активен (дев-сборка);
//    0 — Add/Frame становятся пустыми inline-функциями, PROF_SCOPE исчезает, вся
//        стоимость (мьютекс/хэш/строки/печать) уходит из бинаря. Clock::now()/MsSince
//        остаются реальными (наносекунды), поэтому СКВОЗНЫЕ замеры (submit_time на
//        слоте, fence-wait между функциями) не требуют правок и не дают unused-warning.

#ifndef ENGINE_PROFILE          // на случай сборки без CMake — по умолчанию включён
#define ENGINE_PROFILE 1
#endif

#if ENGINE_PROFILE

class FrameProfiler {
public:
    FrameProfiler(const char* title, double report_period_ms)
        : title(title), report_period_ms(report_period_ms) {}

    // Записать замер: name — метка слота (константа-литерал или debug_name буфера),
    // ms — время, bytes — опциональный размер (0 = не показывать столбец размера).
    void Add(const char* name, double ms, uint64_t bytes = 0);

    // Открыть/закрыть вложенный замер. Родителя определяет НЕ имя, а стек открытых
    // скоупов: PROF_SCOPE зовёт Push на входе и Pop на выходе, поэтому дерево в отчёте
    // точное само собой и не зависит от того, сколько пробелов автор поставил в имени.
    // Add() без Push (замеры, вызываемые напрямую) прикрепляется к скоупу, открытому в
    // этот момент ЭТИМ ЖЕ ПОТОКОМ, — то есть туда, где он и выполняется.
    //
    // Стек — ПО ПОТОКУ (живёт в .cpp), и это не перестраховка: один экземпляр обслуживает
    // несколько потоков. Prof::Render() пишут и render-поток (render_cpu и то, что внутри),
    // и fence-поток (gpu_frame, fence_wait). С общим стеком замеры fence-потока становились
    // детьми открытого render_cpu, и [other] уходил в минус на десятки миллисекунд.
    size_t Push(const char* name);
    void   Pop(size_t index, double ms, uint64_t bytes = 0);

    // Отметка конца кадра секции. Раз в ~report_period_ms реального времени — печать + сброс.
    void Frame();

private:
    struct Slot {
        std::string name;
        size_t   parent = (size_t)-1;   // индекс родителя в slots; -1 = корень
        double   sum_ms = 0.0;
        double   max_ms = 0.0;
        uint64_t calls = 0;
        uint64_t last_bytes = 0;
        bool     has_bytes = false;
    };
    Slot& Touch(const char* name);
    void  PrintAndReset(double window_ms);

    std::mutex mtx;
    std::string title;
    double report_period_ms;
    int frames = 0;
    std::chrono::steady_clock::time_point last_print{};
    bool started = false;
    std::vector<Slot> slots;                     // в порядке первого появления — стабильный вывод
    std::unordered_map<std::string, size_t> index;
};

#else   // ENGINE_PROFILE == 0 — заглушка: все методы пустые inline, компилятор их вырезает

class FrameProfiler {
public:
    FrameProfiler(const char* /*title*/, double /*report_period_ms*/) {}
    void Add(const char* /*name*/, double /*ms*/, uint64_t /*bytes*/ = 0) {}
    size_t Push(const char* /*name*/) { return 0; }
    void   Pop(size_t /*index*/, double /*ms*/, uint64_t /*bytes*/ = 0) {}
    void Frame() {}
};

#endif // ENGINE_PROFILE

namespace Prof {
    using Clock = std::chrono::steady_clock;

    // Секции конвейера (каждую крутит свой поток — писатель у секции один).
    //   SIM    = game_iter + PrepareFunc + обновление буферов (sim-поток)
    //   UPLOAD = UploadFunc: ожидание upload-fence + возврат TB (upload-поток)
    //   RENDER = RenderFunc + завершение кадра в FenceFunc (render/fence-потоки)
    FrameProfiler& Sim();
    FrameProfiler& Upload();
    FrameProfiler& Render();

    inline double MsSince(Clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    // RAII-замер всего {}-блока: t0 берётся в конструкторе, Add() — в деструкторе.
    // При ENGINE_PROFILE=0 макрос PROF_SCOPE вырождается в ((void)0) и этот тип не
    // инстанцируется. ВНИМАНИЕ: меряет ВЕСЬ блок, включая ранний return/break/continue
    // из него (в отличие от ручного Add в конце) — не оборачивать блоки с ранним выходом.
    struct ScopeTimer {
        FrameProfiler& p;
        size_t         idx;
        Clock::time_point t0;
        // Push ДО отсчёта времени: он открывает скоуп, и всё замеренное внутри
        // прикрепится к нему. Pop закрывает и записывает длительность.
        ScopeTimer(FrameProfiler& prof, const char* n) : p(prof), idx(prof.Push(n)), t0(Clock::now()) {}
        ~ScopeTimer() { p.Pop(idx, MsSince(t0)); }
        ScopeTimer(const ScopeTimer&) = delete;
        ScopeTimer& operator=(const ScopeTimer&) = delete;
    };
}

#if ENGINE_PROFILE
    #define PROF_CAT_(a, b) a##b
    #define PROF_CAT(a, b)  PROF_CAT_(a, b)
    // PROF_SCOPE(Sim, "name") — замер до конца текущего {}-блока.
    #define PROF_SCOPE(sec, name) Prof::ScopeTimer PROF_CAT(prof_scope_, __LINE__){ Prof::sec(), name }
    #define PROF_FRAME(sec)       Prof::sec().Frame()
#else
    #define PROF_SCOPE(sec, name) ((void)0)
    #define PROF_FRAME(sec)       ((void)0)
#endif
