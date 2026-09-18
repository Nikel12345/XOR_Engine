#include "PCH.h"
#include "EngineProfiler.h"
#include <cstdio>
#include <functional>
#include <utility>

#if ENGINE_PROFILE

namespace {
// Ищем среди записей СВОЕГО профайлера, а не на вершине: в стеке потока могут лежать
// скоупы чужого экземпляра (см. Push в заголовке).
thread_local std::vector<std::pair<const FrameProfiler*, size_t>> t_open;

size_t OpenParent(const FrameProfiler* p)
{
    for (auto it = t_open.rbegin(); it != t_open.rend(); ++it)
        if (it->first == p) return it->second;
    return (size_t)-1;
}
} // namespace

void FrameProfiler::Add(const char* name, double ms, uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(mtx);
    Slot& s = Touch(name);
    if (s.parent == (size_t)-1) s.parent = OpenParent(this);
    s.sum_ms += ms;
    s.calls  += 1;
    if (ms > s.max_ms) s.max_ms = ms;
    if (bytes) { s.last_bytes = bytes; s.has_bytes = true; }
}

size_t FrameProfiler::Push(const char* name)
{
    std::lock_guard<std::mutex> lk(mtx);
    Slot& s = Touch(name);
    const size_t idx = index[name];
    if (s.parent == (size_t)-1) s.parent = OpenParent(this);
    t_open.push_back({ this, idx });
    return idx;
}

void FrameProfiler::Pop(size_t idx, double ms, uint64_t bytes)
{
    std::lock_guard<std::mutex> lk(mtx);
    for (auto it = t_open.rbegin(); it != t_open.rend(); ++it)
        if (it->first == this) { t_open.erase(std::next(it).base()); break; }
    if (idx >= slots.size()) return;
    Slot& s = slots[idx];
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
    // Непарный Push (их зовут и напрямую, не только из ScopeTimer) чистится здесь, а не копится
    // до конца прогона. Только СВОИ записи — стек общий на поток.
    for (auto it = t_open.begin(); it != t_open.end(); )
        it = (it->first == this) ? t_open.erase(it) : it + 1;
    ++frames;
    auto now = std::chrono::steady_clock::now();
    if (!started) {
        // Первая итерация в окно НЕ входит: в слотах уже лежит стартовая работа (загрузка
        // сцены, первые бейки), а окно отсчитывается только отсюда — сумма фаз оказалась бы
        // больше такта, и [outside scopes] ушёл бы в минус.
        started = true;
        last_print = now;
        for (Slot& s : slots) { s.sum_ms = 0.0; s.max_ms = 0.0; s.calls = 0; }
        frames = 0;
        return;
    }
    double since = std::chrono::duration<double, std::milli>(now - last_print).count();
    if (since < report_period_ms) return;
    PrintAndReset(since);
    last_print = now;
}

void FrameProfiler::PrintAndReset(double window_ms)
{
    // Один fputs на весь отчёт: иначе строки sim- и render-потоков перемешиваются посреди слова.
    // ASCII-only: файл в UTF-8, но MSVC собирает литерал в CP1251 и консоль показывает мусор —
    // это относится и к именам скоупов, которые приходят от вызывающего и идут как есть.
    std::string out;
    char line[256];

    const double tick = frames ? window_ms / (double)frames : 0.0;
    const double rate = window_ms > 0.0 ? (frames * 1000.0 / window_ms) : 0.0;

    const size_t n = slots.size();
    std::vector<double> per_it(n, 0.0);
    std::vector<double> child_sum(n, 0.0);
    std::vector<std::vector<size_t>> kids(n);
    std::vector<size_t> roots;

    for (size_t i = 0; i < n; ++i)
        per_it[i] = frames ? slots[i].sum_ms / (double)frames : 0.0;

    // Родителя записал Push/Add в момент замера — восстанавливать дерево по именам или
    // порядку не нужно.
    for (size_t i = 0; i < n; ++i) {
        const size_t p = slots[i].parent;
        if (p == (size_t)-1 || p >= n) { roots.push_back(i); continue; }
        kids[p].push_back(i);
        child_sum[p] += per_it[i];
    }

    auto row = [&](int level, const char* name, double it_ms, const Slot* s, bool synthetic)
    {
        const double share = tick > 0.0 ? 100.0 * it_ms / tick : 0.0;
        std::string pad((size_t)level * 2, ' ');
        std::string label = pad + name;
        if (label.size() > 44) label.resize(44);

        if (synthetic) {
            std::snprintf(line, sizeof(line), "  %-44s %8.3f %5.1f%%\n",
                label.c_str(), it_ms, share);
        }
        else {
            const double avg = s->calls ? s->sum_ms / (double)s->calls : 0.0;
            char tail[48] = { 0 };
            if (s->has_bytes)
                std::snprintf(tail, sizeof(tail), " %9.1f KB", s->last_bytes / 1024.0);
            std::snprintf(line, sizeof(line),
                "  %-44s %8.3f %5.1f%%  avg %8.3f  max %8.3f  n=%-6lld%s\n",
                label.c_str(), it_ms, share, avg, s->max_ms,
                (long long)s->calls, tail);
        }
        out += line;
    };

    // Ведущие пробелы в именах — рудимент прежней схемы вложенности, ни на что не влияют.
    std::function<void(size_t, int)> emit = [&](size_t i, int level) {
        const std::string& nm = slots[i].name;
        size_t b = nm.find_first_not_of(" =");   // '=' - метка справочного замера, в подписи не нужна
        row(level, nm.c_str() + (b == std::string::npos ? 0 : b), per_it[i], &slots[i], false);
        for (size_t k : kids[i]) emit(k, level + 1);
        if (!kids[i].empty())
            row(level + 1, "[other]", per_it[i] - child_sum[i], nullptr, true);
    };

    std::snprintf(line, sizeof(line),
        "\n===== [%s]  %d iters in %.1f s  |  %.1f/s  |  tick %.3f ms =====\n",
        title.c_str(), frames, window_ms / 1000.0, rate, tick);
    out += line;
    // ДВЕ СЕКЦИИ, и смешивать их нельзя. Фазы разбивают такт, их сумма плюс [outside scopes]
    // даёт 100%. Справочные (имя с '=') меряют соседнее - работу GPU, ожидание в другом потоке,
    // сам такт целиком; они перекрываются и не суммируются. В одном списке получалась чушь
    // вроде "frame_period 100% и рядом outside scopes 96%".
    std::vector<size_t> phases, refs;
    for (size_t r : roots) {
        const std::string& nm = slots[r].name;
        const size_t b = nm.find_first_not_of(' ');
        if (b != std::string::npos && nm[b] == '=') refs.push_back(r);
        else phases.push_back(r);
    }

    std::snprintf(line, sizeof(line), "  %-44s %8s %6s\n", "phases (sum = tick)", "/it ms", "%tick");
    out += line;

    double top_sum = 0.0;
    for (size_t r : phases) { emit(r, 0); top_sum += per_it[r]; }

    row(0, "[outside scopes]", tick - top_sum, nullptr, true);

    if (!refs.empty()) {
        std::snprintf(line, sizeof(line),
            "  %-44s %8s %6s\n", "reference (measured elsewhere, not summed)", "/it ms", "%tick");
        out += line;
        for (size_t r : refs) emit(r, 0);
    }

    out += "==========================================================\n";

    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);

    for (Slot& s : slots) { s.sum_ms = 0.0; s.max_ms = 0.0; s.calls = 0; }
    frames = 0;
}

#endif // ENGINE_PROFILE

namespace Prof {
    // Период в РЕАЛЬНОМ времени, не в кадрах: иначе SIM с низким UPS и RENDER с высоким FPS
    // печатались бы с разной частотой.
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
