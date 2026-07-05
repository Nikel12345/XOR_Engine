#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  EngineProfiler — лёгкий КАДРОВЫЙ профайлер для дебага просадок.
//
//  Каждый именованный слот копит сумму/макс времени (мс) и число вызовов за окно,
//  плюс опциональный «размер» полезной нагрузки (байты) — для буферов. Раз в
//  report_period_ms реального времени Frame() печатает усреднённую сводку и обнуляет
//  окно. Интервал по ВРЕМЕНИ (а не по кадрам): SIM с низким UPS и RENDER с высоким
//  FPS печатаются одинаково регулярно.
//
//  Замер: auto t = Prof::Clock::now(); <работа>; Prof::Sim().Add("name", Prof::MsSince(t));
//  Конец кадра секции: Prof::Sim().Frame();  (печать раз в ~report_period_ms мс)
//
//  Потокобезопасен: sim- и render-потоки пишут в РАЗНЫЕ экземпляры (Prof::Sim /
//  Prof::Render), а Add()/Frame() всё равно под мьютексом — накладные копейки
//  на фоне замеряемых миллисекунд.
// ─────────────────────────────────────────────────────────────────────────────
class FrameProfiler {
public:
    FrameProfiler(const char* title, double report_period_ms)
        : title(title), report_period_ms(report_period_ms) {}

    // Записать замер: name — метка слота (константа-литерал или debug_name буфера),
    // ms — время, bytes — опциональный размер (0 = не показывать столбец размера).
    void Add(const char* name, double ms, uint64_t bytes = 0);

    // Отметка конца кадра секции. Раз в ~report_period_ms реального времени — печать + сброс.
    void Frame();

private:
    struct Slot {
        std::string name;
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
}
