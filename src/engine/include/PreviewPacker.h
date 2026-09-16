#pragma once
#include <SDL3/SDL_gpu.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "ResourceId.h"

struct TextureAtlas;   // источник блита; полный тип нужен только в .cpp

// Замков внутри нет: Request и Blit зовутся с одного потока (sim), а GetUV с UI-потока читает
// живое состояние — тот же размен, что и в остальных панелях редактора.
class PreviewPacker {
public:
    static constexpr uint32_t ATLAS_SIZE = 2048;
    static constexpr uint32_t CELL       = 64;
    static constexpr uint32_t PER_ROW    = ATLAS_SIZE / CELL;   // 32×32 = 1024 ячейки
    static constexpr uint32_t CAPACITY   = PER_ROW * PER_ROW;

    struct UV { bool valid = false; float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };

    void Create(SDL_GPUDevice* dev);
    void Destroy(SDL_GPUDevice* dev);

    void Request(TextureId id, TextureAtlas* src,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer);

    // Звать ПОСЛЕ всех Request кадра. Заявка с ещё не забейканным источником остаётся ждать.
    void Publish();

    void Blit(SDL_GPUCommandBuffer* cb);

    bool HasPendingBlits() const { return !blits_.empty(); }

    UV              GetUV(TextureId id) const;
    SDL_GPUTexture* Texture() const { return atlas_; }

    // Звать только на настоящем удалении текстуры: при замене ячейка обязана пережить её.
    void Release(TextureId id);

private:
    struct Slot {
        int32_t       cell = -1;      // индекс ячейки в сетке (row-major)
        TextureAtlas* src = nullptr;
        uint32_t x = 0, y = 0, w = 0, h = 0, layer = 0;   // регион источника в пикселях
    };
    // Самодостаточная заявка: всё разрешено на Publish, запись читает только эти поля.
    struct BlitTask {
        SDL_GPUTexture* src = nullptr;
        uint32_t sx = 0, sy = 0, sw = 0, sh = 0, layer = 0;
        uint32_t dx = 0, dy = 0;                          // угол ячейки в превью-атласе
    };
    int32_t Alloc();   // фрилист, затем счётчик; -1 если атлас превью полон

    SDL_GPUTexture* atlas_ = nullptr;

    std::unordered_map<TextureId, Slot>    slots_;
    std::vector<TextureId>                 dirty_;       // ждут разрешения в Publish
    std::vector<int32_t>                   free_cells_;
    int32_t                                next_cell_ = 0;

    std::vector<BlitTask>    blits_;
};
