#pragma once
#include <cstdint>
class EngineContext;

// Набор фрактальных апдейтеров игры — зеркало движкового DefaultUpdateSet: каждая Set*Updater
// вешает CreateUpdateInstruction на СВОЙ именованный буфер, а сами буферы создаёт MyGame::MainInit
// (CreateBufferData) под if'ом с именем сцены — там же выбирается и апдейтер. Рендер-ресурсы
// (модель/шейдеры/sp/материал) — ресурсы СЦЕНЫ (saved_scene_*), энтити фона — в её scene.json.
//
// ТРЮК С КАМЕРОЙ (общий для всех фракталов). Камера движка — чистый АККУМУЛЯТОР сдвига в
// игровых юнитах: каждый тик апдейтер забирает GetPosition() как дельту и сбрасывает её в ноль,
// поэтому дельта всегда ~O(1) — полное разрешение float на любой скорости и глубине. Позиция во
// фрактале — собственное состояние апдейтера (double и точнее), масштаб применяется при
// внесении дельты. В шейдер из камеры доезжает максимум ротация (v_dir у 3D); у 2D — ничего.
//
// ГУБКА МЕНГЕРА (scene_fractal): позиция = целочисленный base-3 адрес ячейки + double-локаль
// + дробный масштаб σ; авто-масштаб следует за CPU-зеркалом DE губки. Подробности — у
// SetMengerFrameUpdater в cpp.
//
// МАНДЕЛЬБРОТ (scene_mandelbrot): центр окна — double (резкость ~50 октав, как и у орбиты),
// зум half_h — ПОЛНОСТЬЮ РУЧНОЙ (I/K, без авто-слежения: у тонких нитей множества DE
// проваливается на порядки ниже видимого масштаба и авто-цель утаскивала камеру в точку без
// возможности отдалить — см. FractalUpdateSet.cpp). Каждый тик CPU итерирует РЕФЕРЕНС-орбиту
// центра в double и публикует её буфером; пиксели в шейдере итерируют только дельту
// (перетурбация, ре-базирование Чжуорана).
namespace FractalUpdateSet
{
    // Имена буферов. На них по имени ссылаются fs_buffers sp сцены (shaders.json) — буфер
    // ДОЛЖЕН существовать до LoadScene (резолв имён идёт по уже созданным, Engine_Scene).
    inline constexpr const char* MENGER_FRAME_BUFFER     = "_FractalFrameBuffer";
    inline constexpr const char* MANDELBROT_ORBIT_BUFFER = "_MandelbrotOrbitBuffer";

    // Кадр губки: [0]=(позиция,глубина), [1]=(K,tFar,1/туман,0), [2+j]=(офсет предка, 3^(j+1)).
    inline constexpr uint32_t MENGER_MAX_CONTEXT = 8;   // уровней предков в шейдер (дальше — туман)
    inline constexpr uint32_t MENGER_FRAME_BYTES = (2 + MENGER_MAX_CONTEXT) * 4 * sizeof(float);

    // Кадр Мандельброта: float2-записи — [0]=(len asuint, half_h), [1]=(aspect, 0), [2+n]=Z_n.
    // 4096 итераций референса хватает на весь запас резкости double (~50 октав: 64+96/октаву).
    inline constexpr uint32_t MANDELBROT_ORBIT_MAX   = 4096;
    inline constexpr uint32_t MANDELBROT_ORBIT_BYTES = (2 + MANDELBROT_ORBIT_MAX) * 2 * sizeof(float);

    // Апдейтеры (по одному на фрактал; MainInit зовёт ОДИН — парой к CreateBufferData его буфера).
    void SetMengerFrameUpdater(EngineContext* ctx);
    void SetMandelbrotOrbitUpdater(EngineContext* ctx);

    // Ручной СДВИГ масштаба (удержание I/K): delta_levels — доля уровня за вызов, шаг *= 3^∓delta
    // (+ = «я мельче»/глубже, − = крупнее). У губки — сдвиг окна ±6 уровней вокруг авто-масштаба
    // (сам масштаб следует за расстоянием до стенки); у Мандельброта — half_h НАПРЯМУЮ, весь
    // диапазон (авто-слежения там нет, см. заголовок выше). Общий вызов для обоих фракталов
    // (активен один — второй свою половину состояния просто не читает). Вызывать с sim-потока.
    void FractalScaleStep(float delta_levels);

    // Push-константы фрактальных фонов (fragment слот 0 → b0, space3; раскладка = cbuffer
    // FractalParams обоих фрагментников). ВАЖНО: тело MAIN_PASS передаёт в push_func nullptr
    // вместо данных (RenderPassStandardBody(..., nullptr)), поэтому биндить надо НАПРЯМУЮ
    // полем sp->push_func лямбдой, игнорирующей raw, — BindPushConstants<T> разыменовывает
    // raw и на программах main-пасса падал бы.
    struct alignas(16) FractalPushData {
        float    time      = 0.0f;   // губка: вращение света; Мандельброт: анимация палитры
        // Потолок шагов рэймарча (губка, over-relaxation ω=1.6) либо итераций пикселя
        // (Мандельброт — там пуш ставит MANDELBROT_ORBIT_MAX).
        uint32_t max_steps = 128;
        float    _pad0     = 0.0f;
        float    _pad1     = 0.0f;
    };
}
