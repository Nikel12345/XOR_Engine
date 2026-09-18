#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

//  EngineProfiler — кадровый профайлер: именованные слоты копят время за окно, Frame()
//  раз в report_period_ms печатает дерево и обнуляет окно.
//
//    { PROF_SCOPE(Sim, "name"); <работа> }             — замер всего {}-блока
//    Prof::Sim().Add("name", Prof::MsSince(t0));       — точечный замер
//
//  Окно по ВРЕМЕНИ, а не по кадрам: иначе SIM с низким UPS и RENDER с высоким FPS
//  печатались бы с разной частотой.
//
//  ENGINE_PROFILE=0 опустошает Add/Frame и убирает PROF_SCOPE, но Clock::now()/MsSince
//  остаются рабочими: сквозные замеры (submit_time слота, fence-wait между функциями)
//  живут вне скоупов и правок под шиппинг не требуют.

#ifndef ENGINE_PROFILE          // на случай сборки без CMake — по умолчанию включён
#define ENGINE_PROFILE 1
#endif

#if ENGINE_PROFILE

class FrameProfiler {
public:
    FrameProfiler(const char* title, double report_period_ms)
        : title(title), report_period_ms(report_period_ms) {}

    // bytes = 0 — столбец размера не печатать.
    void Add(const char* name, double ms, uint64_t bytes = 0);

    // Родителя задаёт стек открытых скоупов, а НЕ имя: Add() без Push прикрепляется туда,
    // где выполняется. Стек — ПО ПОТОКУ, и это не перестраховка: один экземпляр обслуживают
    // несколько потоков (Prof::Render() пишут и render-, и fence-поток). С общим стеком
    // замеры fence-потока становились детьми открытого render_cpu, и [other] уходил в минус.
    size_t Push(const char* name);
    void   Pop(size_t index, double ms, uint64_t bytes = 0);

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

#else

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

    //   SIM    = game_iter + PrepareFunc + обновление буферов (sim-поток)
    //   UPLOAD = UploadFunc: ожидание upload-fence + возврат TB (upload-поток)
    //   RENDER = RenderFunc + завершение кадра в FenceFunc (ДВА потока — см. Push)
    FrameProfiler& Sim();
    FrameProfiler& Upload();
    FrameProfiler& Render();

    inline double MsSince(Clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    struct ScopeTimer {
        FrameProfiler& p;
        size_t         idx;
        Clock::time_point t0;
        // idx объявлен ДО t0: порядок инициализации членов держит Push вне отсчёта времени.
        ScopeTimer(FrameProfiler& prof, const char* n) : p(prof), idx(prof.Push(n)), t0(Clock::now()) {}
        ~ScopeTimer() { p.Pop(idx, MsSince(t0)); }
        ScopeTimer(const ScopeTimer&) = delete;
        ScopeTimer& operator=(const ScopeTimer&) = delete;
    };
}

#if ENGINE_PROFILE
    #define PROF_CAT_(a, b) a##b
    #define PROF_CAT(a, b)  PROF_CAT_(a, b)
    #define PROF_SCOPE(sec, name) Prof::ScopeTimer PROF_CAT(prof_scope_, __LINE__){ Prof::sec(), name }
    #define PROF_FRAME(sec)       Prof::sec().Frame()
#else
    #define PROF_SCOPE(sec, name) ((void)0)
    #define PROF_FRAME(sec)       ((void)0)
#endif
