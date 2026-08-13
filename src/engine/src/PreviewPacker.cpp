#include "PCH.h"
#include "PreviewPacker.h"
#include "TextureData.h"
#include "Utils.h"
#include <algorithm>

void PreviewPacker::Create(SDL_GPUDevice* dev)
{
    // Обычный 2D (ImGui сэмплит только texture2D). COLOR_TARGET — приёмник SDL_BlitGPUTexture
    // (блит рендерит в назначение), SAMPLER — чтение ImGui-конвейером.
    SDL_GPUTextureCreateInfo pci{};
    pci.type = SDL_GPU_TEXTURETYPE_2D;
    pci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    pci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    pci.width = ATLAS_SIZE;
    pci.height = ATLAS_SIZE;
    pci.layer_count_or_depth = 1;
    pci.num_levels = 1;
    atlas_ = SDL_CreateGPUTexture(dev, &pci);
    if (!atlas_) SDL_Log("PreviewPacker: creation failed (%s) - UI previews disabled", SDL_GetError());
}

void PreviewPacker::Destroy(SDL_GPUDevice* dev)
{
    if (atlas_) { SDL_ReleaseGPUTexture(dev, atlas_); atlas_ = nullptr; }
    slots_.clear();
    dirty_.clear();
    free_cells_.clear();
    next_cell_ = 0;
    blits_.clear();
}

int32_t PreviewPacker::Alloc()
{
    if (!free_cells_.empty()) { int32_t c = free_cells_.back(); free_cells_.pop_back(); return c; }
    if (next_cell_ < safe_u32t_i(CAPACITY)) return next_cell_++;
    return -1;   // сетка исчерпана
}

void PreviewPacker::Request(const std::string& name, TextureAtlas* src,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer)
{
    if (!atlas_ || name.empty()) return;

    Slot& s = slots_[name];   // существующий переиспользуем (ячейка на месте), новый — заводим
    if (s.cell < 0) {
        s.cell = Alloc();
        if (s.cell < 0) { slots_.erase(name); SDL_Log("PreviewPacker: full - no preview for '%s'", name.c_str()); return; }
    }
    s.src = src; s.x = x; s.y = y; s.w = w; s.h = h; s.layer = layer;

    if (std::find(dirty_.begin(), dirty_.end(), name) == dirty_.end())
        dirty_.push_back(name);
}

void PreviewPacker::Publish()
{
    if (dirty_.empty()) return;

    std::vector<BlitTask>    ready;
    std::vector<std::string> retry;   // источники без готовой GPU-текстуры — оставить на потом
    for (const std::string& name : dirty_) {
        auto it = slots_.find(name);
        if (it == slots_.end() || it->second.cell < 0) continue;   // Release-нут между Request и Publish
        const Slot& s = it->second;
        SDL_GPUTexture* src = s.src ? s.src->texture_binding.texture : nullptr;
        if (!src) { retry.push_back(name); continue; }              // атлас ещё не забейкан

        BlitTask t{};
        t.src = src;
        t.sx = s.x; t.sy = s.y; t.sw = s.w; t.sh = s.h; t.layer = s.layer;
        // Индекс ячейки (row-major) → её пиксельный угол. Считаем здесь, чтобы render получил
        // готовые координаты и не знал ни про сетку, ни про то, что ячейка бывает -1.
        t.dx = safe_i_u32(s.cell % safe_u32t_i(PER_ROW)) * CELL;
        t.dy = safe_i_u32(s.cell / safe_u32t_i(PER_ROW)) * CELL;
        ready.push_back(t);
    }
    dirty_.swap(retry);

    if (ready.empty()) return;
    blits_.insert(blits_.end(), ready.begin(), ready.end());
}

void PreviewPacker::Blit(SDL_GPUCommandBuffer* cb)
{
    // Запись на текстурный cb ПОСЛЕ заливки пикселей и мипов того же cb: порядок внутри cb
    // гарантирует, что источник уже содержит пиксели. ImGui рисует превью-атлас позже и ДРУГИМ
    // cb (render-поток) — там порядок держит уже очередь: этот cb сабмичен раньше кадра.
    if (blits_.empty()) return;
    if (!atlas_) { blits_.clear(); return; }

    for (const BlitTask& t : blits_) {
        SDL_GPUBlitInfo bi{};
        bi.source.texture = t.src;
        bi.source.mip_level = 0;
        bi.source.layer_or_depth_plane = t.layer;
        bi.source.x = t.sx; bi.source.y = t.sy; bi.source.w = t.sw; bi.source.h = t.sh;
        bi.destination.texture = atlas_;
        bi.destination.x = t.dx;
        bi.destination.y = t.dy;
        bi.destination.w = CELL;
        bi.destination.h = CELL;
        bi.load_op = SDL_GPU_LOADOP_LOAD;   // соседние ячейки не трогаем
        // Мелкий источник (меньше ячейки по обеим осям — дефолты 2×2/4×4, иконки) в LINEAR
        // растягивается в мыльный градиент и подмешивает соседей по кромке региона → NEAREST даёт
        // чёткие тексели без утечки. Крупный (уменьшение до CELL) оставляем LINEAR: NEAREST на
        // даунскейле 2048→64 алиасит. Порог — ровно CELL: увеличение → NEAREST, уменьшение → LINEAR.
        bi.filter = (t.sw < CELL && t.sh < CELL) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &bi);
    }
    blits_.clear();
}

PreviewPacker::UV PreviewPacker::GetUV(const std::string& name) const
{
    UV r{};
    auto it = slots_.find(name);
    if (!atlas_ || it == slots_.end() || it->second.cell < 0) return r;
    const float cell = 1.0f / (float)PER_ROW;
    r.valid = true;
    r.u0 = safe_sint32_f(it->second.cell % safe_u32t_i(PER_ROW)) * cell;
    r.v0 = safe_sint32_f(it->second.cell / safe_u32t_i(PER_ROW)) * cell;
    r.u1 = r.u0 + cell;
    r.v1 = r.v0 + cell;
    return r;
}

void PreviewPacker::Release(const std::string& name)
{
    auto it = slots_.find(name);
    if (it == slots_.end()) return;
    if (it->second.cell >= 0) free_cells_.push_back(it->second.cell);
    slots_.erase(it);
    dirty_.erase(std::remove(dirty_.begin(), dirty_.end(), name), dirty_.end());
}
