#include "PCH.h"
#include "PreviewPacker.h"
#include "TextureData.h"
#include "Utils.h"
#include <algorithm>

void PreviewPacker::Create(SDL_GPUDevice* dev)
{
    SDL_GPUTextureCreateInfo pci{};
    pci.type = SDL_GPU_TEXTURETYPE_2D;
    pci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    pci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    pci.width = ATLAS_SIZE;
    pci.height = ATLAS_SIZE;
    pci.layer_count_or_depth = 1;
    pci.num_levels = 1;
    atlas = SDL_CreateGPUTexture(dev, &pci);
    if (!atlas) SDL_Log("PreviewPacker: creation failed (%s) - UI previews disabled", SDL_GetError());
}

void PreviewPacker::Destroy(SDL_GPUDevice* dev)
{
    if (atlas) { SDL_ReleaseGPUTexture(dev, atlas); atlas = nullptr; }
    slots.clear();
    free_cells.clear();
    next_cell = 0;
    blits.clear();
}

int32_t PreviewPacker::Alloc()
{
    if (!free_cells.empty()) { int32_t c = free_cells.back(); free_cells.pop_back(); return c; }
    if (next_cell < safe_u32t_i(CAPACITY)) return next_cell++;
    return -1;   // сетка исчерпана
}

void PreviewPacker::Reserve(TextureId id)
{
    if (!atlas || !id || slots.contains(id)) return;
    const int32_t cell = Alloc();
    if (cell < 0) { SDL_Log("PreviewPacker: full - no preview for texture #%d", id.v); return; }
    slots.emplace(id, cell);
}

void PreviewPacker::Request(TextureId id, TextureAtlas* src,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer)
{
    if (!atlas || !slots.contains(id)) return;

    const BlitTask task{ id, src, x, y, w, h, layer };
    auto it = std::find_if(blits.begin(), blits.end(), [id](const BlitTask& b) { return b.id == id; });
    if (it != blits.end()) *it = task;
    else blits.push_back(task);
}

bool PreviewPacker::HasPendingBlits() const
{
    return std::any_of(blits.begin(), blits.end(),
                       [](const BlitTask& b) { return b.src && b.src->texture_binding.texture; });
}

void PreviewPacker::Blit(SDL_GPUCommandBuffer* cb)
{
    // Писать ПОСЛЕ заливки и мипов того же cb: порядок внутри cb и гарантирует источнику пиксели.
    if (blits.empty()) return;
    if (!atlas) { blits.clear(); return; }

    std::vector<BlitTask> waiting;
    for (const BlitTask& task : blits) {
        auto it = slots.find(task.id);
        if (it == slots.end()) continue;
        SDL_GPUTexture* src = task.src ? task.src->texture_binding.texture : nullptr;
        if (!src) { waiting.push_back(task); continue; }
        const int32_t cell = it->second;

        SDL_GPUBlitInfo info{};
        info.source.texture = src;
        info.source.mip_level = 0;
        info.source.layer_or_depth_plane = task.layer;
        info.source.x = task.x; info.source.y = task.y; info.source.w = task.w; info.source.h = task.h;
        info.destination.texture = atlas;
        info.destination.x = safe_i_u32(cell % safe_u32t_i(PER_ROW)) * CELL;
        info.destination.y = safe_i_u32(cell / safe_u32t_i(PER_ROW)) * CELL;
        info.destination.w = CELL;
        info.destination.h = CELL;
        info.load_op = SDL_GPU_LOADOP_LOAD;   // соседние ячейки не трогаем
        // Увеличение мелкого источника идёт NEAREST (LINEAR размывает его в градиент и тянет
        // соседей по кромке), уменьшение крупного — LINEAR (NEAREST на 2048→64 алиасит).
        info.filter = (task.w < CELL && task.h < CELL) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &info);
    }
    blits.swap(waiting);
}

PreviewPacker::UV PreviewPacker::GetUV(TextureId id) const
{
    UV uv{};
    auto it = slots.find(id);
    if (!atlas || it == slots.end()) return uv;
    const float cell_uv = 1.0f / (float)PER_ROW;
    uv.valid = true;
    uv.u0 = safe_sint32_f(it->second % safe_u32t_i(PER_ROW)) * cell_uv;
    uv.v0 = safe_sint32_f(it->second / safe_u32t_i(PER_ROW)) * cell_uv;
    uv.u1 = uv.u0 + cell_uv;
    uv.v1 = uv.v0 + cell_uv;
    return uv;
}

void PreviewPacker::Release(TextureId id)
{
    auto it = slots.find(id);
    if (it == slots.end()) return;
    free_cells.push_back(it->second);
    slots.erase(it);
    std::erase_if(blits, [id](const BlitTask& b) { return b.id == id; });
}
