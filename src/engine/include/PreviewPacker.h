#pragma once
#include <SDL3/SDL_gpu.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct TextureAtlas;   // источник блита (его GPU-текстура + регион); полный тип нужен только в .cpp

// Подсистема превью ассетов для UI — отдельная от текстурных атласов, по образцу AtlasPacker
// (самодостаточный упаковщик). Один 2D-атлас (ImGui сэмплит только texture2D), нарезанный на
// фиксированную сетку ячеек; регион исходного атласа блитится в ячейку.
//
// КЛЮЧ — ИМЯ текстуры, а НЕ TextureHandle*. Из этого вытекает всё ценное:
//   • TextureHandle не несёт UI-специфичного поля (preview_cell удалён) — он про GPU-раскладку, и только;
//   • пересоздание имени (LoadScene/replace) сохраняет ячейку: слот живёт, пока имя не Release-нут
//     явно, поэтому плитка браузера НЕ мигает затычкой — она показывает прежнюю картинку, пока не
//     ляжет новый блит. Раньше ячейка жила в экземпляре хэндла и терялась на каждом delete+create,
//     а LIFO-фрилист отдавал новому хэндлу ЧУЖУЮ ячейку → рябь по плиткам при загрузке сцены;
//   • UI резолвит превью по имени (GetUV), не через хэндл, — а хэндла нет всё время декода файла.
//
// ПОТОКИ. И Request (_PlaceTask на prepare), и Blit идут с ОДНОГО потока — sim: блит это
// отрисовка, копировальная очередь её не исполнит, поэтому текстурная работа берёт отдельный
// командбуфер ГРАФИЧЕСКОЙ очереди, но остаётся стадией заливки (PrepareFuncPrepassUndepended).
// Замков внутри нет — звать Blit со второго потока нельзя, blits_ порвётся.
// Устройство осталось на заявках, а не на общем состоянии: Publish превращает вызревшие заявки в
// самодостаточные BlitTask (источник уже разрешён в SDL_GPUTexture*, ячейка — в пиксельные
// координаты), а Blit читает ТОЛЬКО поля заявок — ни slots_, ни атласа-источника он не касается.
// Незрелые заявки (атлас-источник ещё не забейкан) остаются в dirty_ — не теряются.
// GetUV намеренно БЕЗ замка: его дёргает UI-поток на каждую плитку браузера, и это тот же
// осознанный компромисс «редактор читает живое», что и в остальных панелях (см. CLAUDE.md).
class PreviewPacker {
public:
    static constexpr uint32_t ATLAS_SIZE = 2048;
    static constexpr uint32_t CELL       = 64;
    static constexpr uint32_t PER_ROW    = ATLAS_SIZE / CELL;   // 32×32 = 1024 ячейки
    static constexpr uint32_t CAPACITY   = PER_ROW * PER_ROW;

    // UV ячейки имени в превью-атласе (для ImGui::Image). valid=false — превью для имени нет.
    struct UV { bool valid = false; float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };

    void Create(SDL_GPUDevice* dev);    // выделить GPU-текстуру (в конструкторе TextureManager)
    void Destroy(SDL_GPUDevice* dev);   // release (в деструкторе TextureManager)

    // Заявка: превью имени должно показывать регион src-атласа (пиксельный прямоугольник + слой).
    // Выделяет ячейку под имя (если ещё нет) и ставит слот в дёрти — блит произойдёт в ближайший
    // Blit. Повторный Request того же имени переиспользует ту же ячейку (перезапись картинки на месте).
    void Request(const std::string& name, TextureAtlas* src,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer);

    // Разрешить дёрти-заявки в самодостаточные BlitTask. Источник без GPU-текстуры (атлас ещё не
    // забейкан) остаётся в дёрти до следующего вызова — не теряется.
    // Звать ПОСЛЕ всех Request кадра (см. TextureManager::PackAtlases).
    void Publish();

    // Записать накопленные блиты в cb и очистить их. Пустая очередь — no-op.
    void Blit(SDL_GPUCommandBuffer* cb);

    // Есть ли вызревшие блиты. Нужен вызывающему, который решает, сабмитить ли cb вообще
    // (см. TextureManager::IsDirty) — сама запись пустой очереди безвредна, сабмит нет.
    bool HasPendingBlits() const { return !blits_.empty(); }

    UV              GetUV(const std::string& name) const;
    SDL_GPUTexture* Texture() const { return atlas_; }

    // Освободить ячейку имени (реальное удаление текстуры из UI/переименование). При replace под
    // тем же именем НЕ звать — иначе вернётся моргание. Нет такого имени — no-op.
    void Release(const std::string& name);

private:
    struct Slot {
        int32_t       cell = -1;      // индекс ячейки в сетке (row-major)
        TextureAtlas* src = nullptr;  // источник блита (резолвится в GPU-текстуру на Blit)
        uint32_t x = 0, y = 0, w = 0, h = 0, layer = 0;   // регион источника в пикселях
    };
    // Самодостаточная заявка на блит: всё разрешено на Publish, запись читает только эти поля.
    struct BlitTask {
        SDL_GPUTexture* src = nullptr;                    // GPU-текстура атласа-источника
        uint32_t sx = 0, sy = 0, sw = 0, sh = 0, layer = 0;   // регион источника в пикселях
        uint32_t dx = 0, dy = 0;                          // левый верхний угол ячейки в превью-атласе
    };
    int32_t Alloc();   // ячейка: фрилист, затем счётчик; -1 если атлас превью полон

    SDL_GPUTexture* atlas_ = nullptr;   // создаётся/уничтожается на init/teardown

    // ── собственность sim ──
    std::unordered_map<std::string, Slot>  slots_;
    std::vector<std::string>               dirty_;       // имена, ждущие разрешения в Publish
    std::vector<int32_t>                   free_cells_;
    int32_t                                next_cell_ = 0;

    // Вызревшие заявки, ждущие записи в cb. Раньше их было два вектора с мьютексом между —
    // передача sim → render; после переезда Blit на sim осталась одна очередь задач. Вектор
    // нужен по-прежнему: разрешение заявки (Publish) и её запись (Blit) — разные моменты кадра.
    std::vector<BlitTask>    blits_;
};
