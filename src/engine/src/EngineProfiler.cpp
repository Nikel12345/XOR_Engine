#include "PCH.h"
#include "EngineProfiler.h"
#include <cstdio>

#if ENGINE_PROFILE

void FrameProfiler::Add(const char* name, double ms, uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(mtx);
    Slot& s = Touch(name);
    s.sum_ms += ms;
    s.calls  += 1;
    if (ms > s.max_ms) s.max_ms = ms;
    if (bytes) { s.last_bytes = bytes; s.has_bytes = true; }
}

FrameProfiler::Slot& FrameProfiler::Touch(const char* name)
{
    auto it = index.find(name);
    if (it != index.end()) return slots[it->second];
    size_t i = slots.size();
    index.emplace(name, i);
    Slot s; s.name = name;
    slots.push_back(std::move(s));
    return slots[i];
}

void FrameProfiler::Frame()
{
    std::lock_guard<std::mutex> lk(mtx);
    ++frames;
    auto now = std::chrono::steady_clock::now();
    if (!started) { started = true; last_print = now; return; }
    double since = std::chrono::duration<double, std::milli>(now - last_print).count();
    if (since < report_period_ms) return;
    PrintAndReset(since);
    last_print = now;
}

void FrameProfiler::PrintAndReset(double window_ms)
{
    // Собираем весь отчёт в одну строку и печатаем одним fputs — так вывод sim и
    // render меньше перемешивается между собой.
    std::string out;
    char line[256];

    // ASCII-only в этом файле: он создан как UTF-8, а MSVC читает как CP1251 —
    // кириллица в его строковых литералах вышла бы мусором в консоли.
    double rate = window_ms > 0.0 ? (frames * 1000.0 / window_ms) : 0.0;   // frames/s (FPS/UPS)
    std::snprintf(line, sizeof(line),
        "\n===== [%s]  %d iters in %.1f s  (%.1f/s)  avg=per-call, /it=per-iteration =====\n",
        title.c_str(), frames, window_ms / 1000.0, rate);
    out += line;

    for (const Slot& s : slots) {
        // avg    — среднее на ОДИН вызов (sum/calls): честное время операции;
        // per_it — амортизированное на итерацию секции (sum/frames): вклад в бюджет.
        // Различаются, когда слот срабатывает НЕ каждую итерацию (напр. prep без слота):
        // тогда per_it занижено относительно avg. calls показан как (n=...).
        double avg    = s.calls ? s.sum_ms / (double)s.calls : 0.0;
        double per_it = frames  ? s.sum_ms / (double)frames  : 0.0;
        if (s.has_bytes) {
            std::snprintf(line, sizeof(line),
                "  %-36s avg %8.3f ms  /it %8.3f ms  max %8.3f ms  %9.1f KB  (n=%llu)\n",
                s.name.c_str(), avg, per_it, s.max_ms, s.last_bytes / 1024.0,
                (unsigned long long)s.calls);
        } else {
            std::snprintf(line, sizeof(line),
                "  %-36s avg %8.3f ms  /it %8.3f ms  max %8.3f ms               (n=%llu)\n",
                s.name.c_str(), avg, per_it, s.max_ms, (unsigned long long)s.calls);
        }
        out += line;
    }
    out += "==========================================================\n";

    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);

    // Сброс окна: имена/порядок слотов сохраняем, обнуляем только счётчики.
    for (Slot& s : slots) { s.sum_ms = 0.0; s.max_ms = 0.0; s.calls = 0; }
    frames = 0;
}

#endif // ENGINE_PROFILE

namespace Prof {
    // Период отчёта в мс (реальное время, не кадры). Больше значение — реже вывод.
    FrameProfiler& Sim()
    {
        static FrameProfiler p("SIM / UPDATE LOOP", 10000.0);   // отчёт раз в ~10 c
        return p;
    }
    FrameProfiler& Upload()
    {
        static FrameProfiler p("UPLOAD LOOP", 10000.0);
        return p;
    }
    FrameProfiler& Render()
    {
        static FrameProfiler p("RENDER LOOP", 10000.0);
        return p;
    }
}
