#include "PCH.h"
#include "TextureManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "finders_interface.h"

struct AtlasPacker {
    struct Layer {
        std::vector<rectpack2D::space_rect> free_spaces;

        void reset(int width, int height) {
            free_spaces.clear();
            free_spaces.push_back(rectpack2D::rect_xywh(0, 0, width, height));
        }

        bool empty(int width, int height) const {
            return free_spaces.size() == 1 && free_spaces[0].x == 0 && free_spaces[0].y == 0
                && free_spaces[0].w == width && free_spaces[0].h == height;
        }

        std::optional<rectpack2D::rect_xywh> insert(int width, int height) {
            using namespace rectpack2D;
            for (int i = (int)free_spaces.size() - 1; i >= 0; --i) {
                const space_rect candidate = free_spaces[i];
                const auto splits = insert_and_split(rect_wh(width, height), candidate);
                if (!splits) continue;
                free_spaces[i] = free_spaces.back();
                free_spaces.pop_back();
                for (int split = 0; split < splits.count; ++split)
                    free_spaces.push_back(splits.spaces[split]);
                return rect_xywh(candidate.x, candidate.y, width, height);
            }
            return std::nullopt;
        }

        void carve(rectpack2D::rect_xywh occupied) {
            using namespace rectpack2D;
            std::vector<space_rect> remaining;
            remaining.reserve(free_spaces.size() + 3);
            for (const space_rect& free_rect : free_spaces) {
                const int left   = std::max(free_rect.x, occupied.x);
                const int top    = std::max(free_rect.y, occupied.y);
                const int right  = std::min(free_rect.x + free_rect.w, occupied.x + occupied.w);
                const int bottom = std::min(free_rect.y + free_rect.h, occupied.y + occupied.h);
                if (right <= left || bottom <= top) { remaining.push_back(free_rect); continue; }
                if (left > free_rect.x)
                    remaining.push_back(rect_xywh(free_rect.x, free_rect.y, left - free_rect.x, free_rect.h));
                if (right < free_rect.x + free_rect.w)
                    remaining.push_back(rect_xywh(right, free_rect.y, free_rect.x + free_rect.w - right, free_rect.h));
                if (top > free_rect.y)
                    remaining.push_back(rect_xywh(left, free_rect.y, right - left, top - free_rect.y));
                if (bottom < free_rect.y + free_rect.h)
                    remaining.push_back(rect_xywh(left, bottom, right - left, free_rect.y + free_rect.h - bottom));
            }
            free_spaces.swap(remaining);
        }
    };
    std::vector<Layer> layers;
};

TextureManager::TextureManager(SDL_GPUDevice* device, TransferManager* transfer_manager): dev(device), trm(transfer_manager){
    using namespace DefaultSamplersNames;
    // У материального сэмплера анизотропия выключена намеренно: она выбирает LOD по резкой оси
    // футпринта и держит высокочастотную нормаль острой, сводя на нет мип-префильтр, из-за чего
    // шейдинг нормали мерцает при движении камеры.
    CreateSampler(DEFAULT_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::DEFAULT_SAMPLER));
    CreateSampler(DEFAULT_SHADOW_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SHADOW_SAMPLER));
	CreateSampler(VSM_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::VSM_SAMPLER));
	CreateSampler(ENV_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::ENV_SAMPLER));
    CreateSampler(SIMPLE_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SIMPLE_SAMPLER));

    {
        SDL_GPUTextureCreateInfo tci{};
        tci.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        tci.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tci.width                = 2048;
        tci.height               = 2048;
        tci.layer_count_or_depth = 1;
        tci.num_levels           = 1;
        tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        TextureAtlas* text_atlas = CreateTextureAtlas(DefaultAtlasNames::TEXT_ATLAS, tci, GetSampler("SimpleSampler"), ResourceTag::Default | ResourceTag::System);
        text_atlas->padding = 0;
    }

    preview.Create(dev);
}

TextureAtlas* TextureManager::CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler, ResourceTag tags)
{
	const AtlasId id = atlases_data.Intern(name);
    if (TextureAtlas* existing = atlases_data.Get(id)) {
        SDL_Log("Texture atlas '%s' already exists, returning existing atlas.", name.c_str());
        return existing;
	}
	auto atlas = std::make_unique<TextureAtlas>();

    atlas->tci = tci;
    atlas->name = name;
    atlas->tags = tags;
    atlas->tci.usage &= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (tci.num_levels > 1)
        atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    atlas->texture_binding.sampler = sampler;
    atlas->width = tci.width;
    atlas->height = tci.height;
    atlas->layers = tci.layer_count_or_depth;
    atlas->padding = (tci.num_levels > 1) ? 16 : 0;
    atlas->mip_levels = tci.num_levels;
    atlas->format = tci.format;
    atlas->texture_type = tci.type;

	TextureAtlas* ptr = atlas.get();
	atlases_data.Put(id, std::move(atlas));
	pending_atlas_bakes.push_back(ptr);
	return ptr;
}

TextureAtlas* TextureManager::CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler, ResourceTag tags)
{
    if (!existing_atlas) {
        SDL_Log("Invalid existing atlas provided for new atlas '%s'", name.c_str());
        return nullptr;
    }
    const AtlasId id = atlases_data.Intern(name);
    if (TextureAtlas* existing = atlases_data.Get(id)) {
        SDL_Log("Texture atlas '%s' already exists, returning existing atlas.", name.c_str());
        return existing;
    }
    auto atlas = std::make_unique<TextureAtlas>();

    atlas->shares_with = existing_atlas;
    atlas->tci = existing_atlas->tci;
    atlas->name = name;
    atlas->tags = tags;
    atlas->texture_binding.sampler = sampler;
    atlas->width = existing_atlas->width;
    atlas->height = existing_atlas->height;
    atlas->layers = existing_atlas->layers;
    atlas->padding = existing_atlas->padding;
    atlas->mip_levels = existing_atlas->mip_levels;
    atlas->texture_type = existing_atlas->texture_type;
    atlas->format = existing_atlas->format;

    TextureAtlas* ptr = atlas.get();
    atlases_data.Put(id, std::move(atlas));
    pending_atlas_bakes.push_back(ptr);
	return ptr;
}

void TextureManager::BakePending()
{
    if (pending_atlas_bakes.empty()) return;

    for (TextureAtlas* atlas : pending_atlas_bakes)
        if (atlas && atlas->shares_with)
            atlas->shares_with->tci.usage |= atlas->tci.usage;

    for (TextureAtlas* atlas : pending_atlas_bakes) {
        if (!atlas || atlas->shares_with || atlas->texture_binding.texture) continue;

        if (atlas->tci.usage == 0) continue;
        atlas->texture_binding.texture = CreateGPU_Texture(atlas->tci);
        if (!atlas->texture_binding.texture)
            SDL_Log("TextureManager::BakePending: atlas creation failed: %s", SDL_GetError());
        else
            SDL_SetGPUTextureName(dev, atlas->texture_binding.texture, atlas->name.c_str()); 
    }
    for (TextureAtlas* atlas : pending_atlas_bakes) {
        if (!atlas || !atlas->shares_with || atlas->texture_binding.texture) continue;
        atlas->texture_binding.texture = atlas->shares_with->texture_binding.texture;
        if (!atlas->texture_binding.texture)
            SDL_Log("TextureManager::BakePending: shared atlas has no source texture.");
    }

    std::erase_if(pending_atlas_bakes, [](TextureAtlas* atlas) { return !atlas || atlas->texture_binding.texture; });
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span, ResourceTag tags)
{
	const AtlasId atlas_id = atlases_data.Find(atlas_name);
	TextureAtlas* atlas = atlases_data.Get(atlas_id);
    if (!atlas) {
        SDL_Log("Texture atlas '%s' not found for texture '%s'", atlas_name.c_str(), name.c_str());
        return nullptr;
	}
	TextureHandle* handle = CreateTexture(name, atlas, w, h, std::move(pixels), layer_span, tags);
	if (handle) handle->atlas_id = atlas_id;
	return handle;
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span, ResourceTag tags)
{
	if (!atlas) {
        SDL_Log("Invalid atlas provided for texture '%s'", name.c_str());
        return nullptr;
	}
	if (layer_span == 0) layer_span = 1;

	const TextureId id = handles_data.Intern(name);
    if (TextureHandle* existing = handles_data.Get(id)) {
        SDL_Log("Texture '%s' already exists, returning existing texture.", name.c_str());
        return existing;
    }

    auto owned_handle = std::make_shared<TextureHandle>();
    owned_handle->atlas = atlas;

    TextureHandle* handle = owned_handle.get();
    handle->width = w;
    handle->height = h;
    handle->tags = tags;
    handle->texture_data.layer_span = layer_span;
    handles_data.Put(id, std::move(owned_handle));
	atlas->textures.push_back(&handle->texture_data);


	CreateUploadTask(id, handle, w, h, std::move(pixels), name, layer_span);

	return handle;
}

SDL_GPUTexture* TextureManager::CreateGPU_Texture(SDL_GPUTextureCreateInfo tci)
{
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &tci);
    return tex;
}

std::vector<std::byte> TextureManager::SurfaceToPixels(SDL_Surface* surface, SDL_PixelFormat format)
{
    std::vector<std::byte> out;
    if (!surface) return out;

    SDL_Surface* converted = SDL_ConvertSurface(surface, format);
    if (!converted) { SDL_Log("TextureManager::SurfaceToPixels: SDL_ConvertSurface failed: %s", SDL_GetError()); return out; }

    const int    width     = converted->w, height = converted->h;
    const size_t bpp       = SDL_BYTESPERPIXEL(format);
    const size_t row_bytes = static_cast<size_t>(width) * bpp;
    out.resize(row_bytes * height);
    const uint8_t* src = static_cast<const uint8_t*>(converted->pixels);
    for (int y = 0; y < height; ++y)   // построчно: pitch поверхности ≥ плотной строки
        SDL_memcpy(out.data() + static_cast<size_t>(y) * row_bytes,
                   src + static_cast<size_t>(y) * converted->pitch, row_bytes);
    SDL_DestroySurface(converted);
    return out;
}

void TextureManager::QueueDeleteTexture(SDL_GPUTexture* texture)
{
    if (!texture) return;   // зовётся ТОЛЬКО render-потоком (владелец очереди — см. поле texture_trash)
    texture_trash.push_back({ texture });
}

void TextureManager::TrashTextures(uint64_t fences_done)
{
    auto it = texture_trash.begin();
    while (it != texture_trash.end()) {
        if (it->ready_at == 0) { it->ready_at = fences_done + BUFFERING_LEVEL; ++it; }
        else if (fences_done >= it->ready_at) {
            SDL_ReleaseGPUTexture(dev, it->tex);
            it = texture_trash.erase(it);
        }
        else ++it;
    }
}

void TextureManager::CreateResizeInstruction(const std::string& texture_name, TextureResizeFunc fn)
{
    resize_instructions_[texture_name] = std::move(fn); 
}

void TextureManager::ExecuteResizeInstructions(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return;
    for (auto& [name, fn] : resize_instructions_)
        if (fn) fn(*this, w, h);
}

void TextureManager::RecreateAtlasTexture(TextureAtlas* atlas, SDL_GPUTextureCreateInfo tci)
{
    if (!atlas) return;

    tci.usage = atlas->tci.usage;
    SDL_GPUTexture* old_tex = atlas->texture_binding.texture;
    atlas->texture_binding.texture = CreateGPU_Texture(tci);
    atlas->tci    = tci;
    atlas->width  = tci.width;
    atlas->height = tci.height;
    QueueDeleteTexture(old_tex);
}

static uint16_t PackUnorm16(float value) {
    return safe_u32_u16(safe_f_u32(std::floor(SDL_clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f)));
}
static float UnpackUnorm16(uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

void TextureManager::CreateUploadTask(TextureId id, TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name, uint32_t layer_span)
{
    UploadTaskTexture task;
    task.id = id;
    task.name = name;
    task.pixels = std::move(pixels);
    task.target_handle = handle;
    task.width = w;
    task.height = h;
    task.layer_span = layer_span;
    task.size = (uint32_t)task.pixels.size();
    texture_upload_tasks.push_back(std::move(task));
}

bool TextureManager::_PlaceTask(UploadTaskTexture& task) {
    using namespace rectpack2D;
    if (task.placed) return true;

    TextureAtlas* atlas = task.target_handle->atlas;
    task.atlas = atlas;
    auto& packer_uptr = atlas_packers[atlas];
    if (!packer_uptr) packer_uptr = std::make_unique<AtlasPacker>();
    AtlasPacker& packer = *packer_uptr;

    const uint32_t src_width = task.width, src_height = task.height;
    if (src_width == 0 || src_height == 0) { task.placed = true; return true; }

    if ((atlas->texture_type == SDL_GPU_TEXTURETYPE_CUBE
      || atlas->texture_type == SDL_GPU_TEXTURETYPE_CUBE_ARRAY) && task.layer_span == 1) {
        SDL_Log("Task '%s': single-layer texture rejected from cube atlas '%s' - its layers are cube "
                "FACES; load the file as a cubemap (scene entry needs \"cube\": true)",
                task.name.c_str(), atlas->name.c_str());
        return false;
    }

    if (task.layer_span > 1) {
        const uint32_t span = task.layer_span;
        // Полнослойность — инвариант: сядь запись в остаток, освобождение вернуло бы чужое место.
        if (src_width != atlas->width || src_height != atlas->height) {
            SDL_Log("Task '%s': %u-layer upload must be full-layer sized (%ux%u, atlas layer %ux%u)",
                    task.name.c_str(), span, src_width, src_height, atlas->width, atlas->height);
            return false;
        }

        // База кратна span: у cube-array это держит грани одного куба в одном слоте массива.
        uint32_t base = UINT32_MAX;
        for (uint32_t first = 0; first + span <= packer.layers.size(); first += span) {
            bool all_free = true;
            for (uint32_t i = 0; i < span && all_free; ++i)
                all_free = packer.layers[first + i].empty((int)atlas->width, (int)atlas->height);
            if (all_free) { base = first; break; }
        }
        if (base == UINT32_MAX) {
            const uint32_t start = safe_u32((packer.layers.size() + span - 1) / span) * span;
            if (start + span <= atlas->layers) {
                while (packer.layers.size() < start + span) {
                    AtlasPacker::Layer empty_layer;
                    empty_layer.reset((int)atlas->width, (int)atlas->height);
                    packer.layers.push_back(std::move(empty_layer));
                }
                base = start;
            }
        }
        if (base == UINT32_MAX) {
            SDL_Log("Failed to pack task '%s' (%u layers) - atlas '%s' full", task.name.c_str(), span, atlas->name.c_str());
            return false;
        }
        for (uint32_t i = 0; i < span; ++i)
            packer.layers[base + i].free_spaces.clear();

        TextureData& placement = task.target_handle->texture_data;
        placement.uv_offset_x = 0;
        placement.uv_offset_y = 0;
        placement.uv_scale_x  = PackUnorm16(1.0f);   // != 0 → запись размещена
        placement.uv_scale_y  = PackUnorm16(1.0f);
        placement.layer = base;

        preview.Request(task.id, atlas, 0, 0, src_width, src_height, base);

        task.dst.texture   = atlas->texture_binding.texture;
        task.dst.x         = 0;
        task.dst.y         = 0;
        task.dst.z         = 0;
        task.dst.layer     = base;
        task.dst.mip_level = 0;
        task.dst.w         = src_width;
        task.dst.h         = src_height;
        task.dst.d         = 1;

        task.placed = true;
        return true;
    }

    // pad_x/pad_y — чистая функция от (размер, атлас), повторённая в _DecodeOuterRect: декод
    // обязан дать РОВНО уложенный здесь прямоугольник, иначе освобождение вернёт чужое место.
    const uint32_t pad_x = (src_width  >= atlas->width)  ? 0 : atlas->padding;
    const uint32_t pad_y = (src_height >= atlas->height) ? 0 : atlas->padding;

    uint32_t placed_layer = 0;
    rect_xywh outer{};

    auto try_place = [&](uint32_t outer_width, uint32_t outer_height, bool allow_new_layer) -> bool {
        for (uint32_t layer = 0; layer < packer.layers.size(); ++layer)
            if (auto placed = packer.layers[layer].insert((int)outer_width, (int)outer_height)) {
                placed_layer = layer; outer = *placed; return true;
            }
        if (allow_new_layer && packer.layers.size() < atlas->layers) {
            AtlasPacker::Layer empty_layer;
            empty_layer.reset((int)atlas->width, (int)atlas->height);
            if (auto placed = empty_layer.insert((int)outer_width, (int)outer_height)) {
                packer.layers.push_back(std::move(empty_layer));
                placed_layer = (uint32_t)packer.layers.size() - 1;
                outer = *placed;
                return true;
            }
        }
        return false;
    };

    if (!try_place(src_width + pad_x * 2, src_height + pad_y * 2, /*allow_new_layer=*/true)) {
        SDL_Log("Failed to pack task '%s' (%ux%u) - atlas full", task.name.c_str(), src_width, src_height);
        return false;
    }

    TextureData& placement = task.target_handle->texture_data;
    placement.uv_offset_x = PackUnorm16((float)(outer.x + pad_x) / (float)atlas->width);
    placement.uv_offset_y = PackUnorm16((float)(outer.y + pad_y) / (float)atlas->height);
    placement.uv_scale_x  = PackUnorm16((float)src_width  / (float)atlas->width);
    placement.uv_scale_y  = PackUnorm16((float)src_height / (float)atlas->height);
    placement.layer = placed_layer;

    preview.Request(task.id, atlas, outer.x + pad_x, outer.y + pad_y, src_width, src_height, placed_layer);

    // Рамка заполняется репликацией кромки: незаполненную мип-генерация подмешала бы в тайл
    // тёмной каймой по периметру меша.
    if (pad_x > 0 || pad_y > 0) {
        const uint32_t bpp           = (uint32_t)(task.pixels.size() / ((size_t)src_width * src_height));
        const uint32_t padded_width  = src_width  + pad_x * 2;
        const uint32_t padded_height = src_height + pad_y * 2;
        std::vector<std::byte> padded((size_t)padded_width * padded_height * bpp);
        for (uint32_t y = 0; y < padded_height; ++y) {
            const uint32_t src_y = (uint32_t)SDL_clamp((int)y - (int)pad_y, 0, (int)src_height - 1);
            const std::byte* src_row = task.pixels.data() + (size_t)src_y * src_width * bpp;
            std::byte* dst_row = padded.data() + (size_t)y * padded_width * bpp;
            for (uint32_t x = 0; x < pad_x; ++x)
                SDL_memcpy(dst_row + (size_t)x * bpp, src_row, bpp);
            SDL_memcpy(dst_row + (size_t)pad_x * bpp, src_row, (size_t)src_width * bpp);
            for (uint32_t x = 0; x < pad_x; ++x)
                SDL_memcpy(dst_row + ((size_t)pad_x + src_width + x) * bpp,
                           src_row + (size_t)(src_width - 1) * bpp, bpp);
        }
        task.pixels = std::move(padded);
        task.width  = padded_width;
        task.height = padded_height;
        task.size   = (uint32_t)task.pixels.size();
    }

    task.dst.texture   = atlas->texture_binding.texture;
    task.dst.x         = (Uint32)outer.x;
    task.dst.y         = (Uint32)outer.y;
    task.dst.z         = 0;
    task.dst.layer     = placed_layer;
    task.dst.mip_level = 0;
    task.dst.w         = task.width;
    task.dst.h         = task.height;
    task.dst.d         = 1;

    task.placed = true;
    return true;
}

static rectpack2D::rect_xywh _DecodeOuterRect(const TextureData& placement, const TextureAtlas* atlas) {
    if (placement.uv_scale_x == 0) return rectpack2D::rect_xywh(0, 0, 0, 0);
    const int inner_width  = (int)(UnpackUnorm16(placement.uv_scale_x)  * atlas->width  + 0.5f);
    const int inner_height = (int)(UnpackUnorm16(placement.uv_scale_y)  * atlas->height + 0.5f);
    const int inner_x      = (int)(UnpackUnorm16(placement.uv_offset_x) * atlas->width  + 0.5f);
    const int inner_y      = (int)(UnpackUnorm16(placement.uv_offset_y) * atlas->height + 0.5f);
    const int pad_x = ((uint32_t)inner_width  >= atlas->width)  ? 0 : atlas->padding;
    const int pad_y = ((uint32_t)inner_height >= atlas->height) ? 0 : atlas->padding;

    const int outer_left   = std::max(0, inner_x - pad_x);
    const int outer_top    = std::max(0, inner_y - pad_y);
    const int outer_right  = std::min((int)atlas->width,  inner_x + inner_width  + pad_x);
    const int outer_bottom = std::min((int)atlas->height, inner_y + inner_height + pad_y);
    return rectpack2D::rect_xywh(outer_left, outer_top,
                                 outer_right - outer_left, outer_bottom - outer_top);
}

void TextureManager::_ReleasePendingRegions() {
    // Обязано идти ПЕРЕД размещением: только так новые текстуры кадра садятся в место, которое
    // освободили снятые.
    for (const auto& [atlas, layer] : pending_region_release_) {
        auto packer_it = atlas_packers.find(atlas);
        if (packer_it == atlas_packers.end() || !packer_it->second) continue;
        auto& layers = packer_it->second->layers;
        if (layer >= layers.size()) continue;

        auto& layer_space = layers[layer];
        layer_space.reset((int)atlas->width, (int)atlas->height);
        // Диапазон, а не равенство: многослойная запись значится только на своём базовом слое, и
        // по равенству остальные её слои стали бы «свободны» под живыми пикселями.
        for (TextureData* placement : atlas->textures)
            if (layer >= placement->layer && layer < placement->layer + placement->layer_span) {
                const rectpack2D::rect_xywh outer = _DecodeOuterRect(*placement, atlas);
                if (outer.w > 0 && outer.h > 0) layer_space.carve(outer);
            }
    }
    pending_region_release_.clear();
}

void TextureManager::_BuildUploadTasks() {
    for (auto& task : texture_upload_tasks)
        _PlaceTask(task);

    // Расхождение размера с форматом назначения не поймает валидация: будет сдвиг строк или
    // чтение за границей transfer-буфера.
    for (auto& task : texture_upload_tasks) {
        const TextureAtlas* atlas = task.atlas;
        if (!atlas) continue;
        const uint32_t expected =
            SDL_CalculateGPUTextureFormatSize(atlas->format, task.width, task.height, task.layer_span);
        if (task.size != expected)
            SDL_Log("Upload '%s': %u байт, а формат атласа '%s' ждёт %u (%ux%u, %u слоёв)",
                task.name.c_str(), task.size, atlas->name.c_str(), expected,
                task.width, task.height, task.layer_span);
    }
}

TransferBufferData* TextureManager::ExecuteUploadTasks(SDL_GPUCopyPass* copy_pass) {
    if (texture_upload_tasks.empty())
        return nullptr;


    constexpr SDL_GPUTextureUsageFlags kMipUsage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // Смещение обязано быть кратно текселю формата НАЗНАЧЕНИЯ: в одном буфере едут R8-глифы и
    // BGRA-тайлы, и нечётная задача сбивает выравнивание всем следующим за ней.
    uint32_t offset = 0;
    for (auto& task : texture_upload_tasks) {
        const uint32_t align = task.atlas ? SDL_GPUTextureFormatTexelBlockSize(task.atlas->format) : 16;
        if (align > 1) offset = (offset + align - 1) / align * align;
        task.offset = offset;
        offset += task.size;
    }
    const uint32_t total = offset;

    TransferBufferData* transfer = total ? trm->AcquireUploadTB(total) : nullptr;
    if (!transfer) {
        // Аренда не удалась — задачи ОСТАВЛЯЕМ: места в атласе им уже розданы и не вернутся,
        // так что потерянные пиксели остались бы мусором в этих регионах навсегда.
        if (total == 0) texture_upload_tasks.clear();
        return nullptr;
    }

    std::unordered_set<SDL_GPUTexture*> mip_decided;

    for (auto& task : texture_upload_tasks) {
        if (!task.dst.texture) continue;
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = transfer->tb;
        src.offset = task.offset;
        src.pixels_per_row = task.width;    // размер ОДНОГО слоя: слои лежат стопкой
        src.rows_per_layer = task.height;

        std::byte* mapped = static_cast<std::byte*>(transfer->mapped);
        SDL_memcpy(mapped + task.offset, task.pixels.data(), task.size);

        // Копия — ПО СЛОЮ: многослойного копирования в API нет вовсе.
        const uint32_t span = task.layer_span ? task.layer_span : 1;
        const uint32_t layer_bytes = task.size / span;
        for (uint32_t i = 0; i < span; ++i) {
            SDL_GPUTextureTransferInfo layer_src = src;
            layer_src.offset = task.offset + i * layer_bytes;
            SDL_GPUTextureRegion layer_dst = task.dst;
            layer_dst.layer = task.dst.layer + i;
            SDL_UploadToGPUTexture(copy_pass, &layer_src, &layer_dst, false);
        }

        const TextureAtlas* atlas = task.atlas;
        if (!atlas || atlas->mip_levels <= 1) continue;
        if (!mip_decided.insert(task.dst.texture).second) continue;

        if ((atlas->tci.usage & kMipUsage) != kMipUsage) {
            SDL_Log("TextureManager::ExecuteUploadTasks: atlas has num_levels=%u but usage lacks "
                    "SAMPLER|COLOR_TARGET - mip generation skipped (would abort on SDL assert).",
                    atlas->mip_levels);
            continue;
        }
        mip_tasks.insert(task.dst.texture);
    };

    texture_upload_tasks.clear();
    return transfer;
}

void TextureManager::GenerateMipmaps(SDL_GPUCommandBuffer* cb)
{
    for (SDL_GPUTexture* tex : mip_tasks)
        SDL_GenerateMipmapsForGPUTexture(cb, tex);
    mip_tasks.clear();
}

SDL_GPUSampler* TextureManager::CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci)
{
    SDL_GPUSampler* s = SDL_CreateGPUSampler(dev, &sci);
    samplers_data[name] = s;
    return s;
}

SDL_GPUSampler* TextureManager::GetSampler(const std::string& name)
{
    auto it = samplers_data.find(name);
    if (it != samplers_data.end()) {
		return it->second;
        }
    else {
        SDL_Log("Sampler '%s' not found", name.c_str());
        return nullptr;
    }
}

bool TextureManager::DeleteTextureHandle(TextureId id, NameSlot slot)
{
    TextureHandle* handle = handles_data.Get(id);
    if (!handle) {
        SDL_Log("Texture handle '%s' not found, cannot delete", handles_data.NameOf(id).c_str());
        return false;
    }
    TextureData* placement = &handle->texture_data;

    if (TextureAtlas* atlas = handle->atlas) {
        // Немедленно и именно здесь: atlas->textures держит указатель внутрь хэндла, который
        // умрёт ниже.
        auto& placements = atlas->textures;
        placements.erase(std::remove(placements.begin(), placements.end(), placement), placements.end());

        for (uint32_t i = 0; placement->uv_scale_x != 0 && i < placement->layer_span; ++i) {
            const std::pair<TextureAtlas*, uint32_t> key{ atlas, placement->layer + i };
            if (std::find(pending_region_release_.begin(), pending_region_release_.end(), key)
                == pending_region_release_.end())
                pending_region_release_.push_back(key);
        }
    }

    texture_upload_tasks.erase(
        std::remove_if(texture_upload_tasks.begin(), texture_upload_tasks.end(),
                       [handle](const UploadTaskTexture& t) { return t.target_handle == handle; }),
        texture_upload_tasks.end());

    // Превью не трогаем: при замене его ячейка обязана пережить удаление, иначе плитка мигнёт.
    // Настоящее удаление освобождает её отдельным ReleasePreview у вызывающего.
    return slot == NameSlot::Release ? handles_data.Drop(id) : handles_data.Clear(id);
}

bool TextureManager::RenameTexture(TextureId id, const std::string& new_name)
{
    if (!handles_data.Get(id) || new_name.empty()) return false;
    if (!handles_data.Rename(id, new_name)) {
        SDL_Log("RenameTexture: '%s' is already taken", new_name.c_str());
        return false;
    }
    return true;
}

size_t TextureManager::ClearSceneTextures()
{
    std::vector<TextureId> doomed;
    for (int32_t i = 0; i < handles_data.Count(); ++i) {
        const TextureHandle* h = handles_data.At(i).object.get();
        if (h && !HasTag(h->tags, ResourceTag::CodeOwned) && !h->source_path.empty())
            doomed.push_back(TextureId{ i });
    }
    for (TextureId id : doomed) { DeleteTextureHandle(id, NameSlot::Release); ReleasePreview(id); }
    return doomed.size();
}

size_t TextureManager::LoadSceneTextures(const std::vector<SceneTextureEntry>& entries,
    const std::function<TextureHandle*(const SceneTextureEntry&)>& create_from_file)
{
    size_t created = 0;
    for (const SceneTextureEntry& e : entries) {
        if (e.name.empty() || e.atlas.empty() || e.path.empty()) {
            SDL_Log("LoadSceneTextures: incomplete entry ('%s') - skipped", e.name.c_str());
            continue;
        }
        // Имя кодового ресурса сцене не отдаём: он пережил бы снос (ClearScene*), но запись
        // манифеста заняла бы его ячейку и подменила содержимое.
        if (const TextureHandle* live = handles_data.Get(handles_data.Find(e.name));
            live && HasTag(live->tags, ResourceTag::CodeOwned)) {
            SDL_Log("LoadSceneTextures: '%s' is code-owned - entry skipped", e.name.c_str());
            continue;
        }
        // Без снятия CreateTexture вернул бы существующий хэндл и заливки не случилось бы.
        if (const TextureId id = handles_data.Find(e.name))
            DeleteTextureHandle(id, NameSlot::Keep);
        if (create_from_file(e)) ++created;
        else SDL_Log("LoadSceneTextures: failed to create '%s' from '%s'", e.name.c_str(), e.path.c_str());
    }
    return created;
}

void TextureManager::DeleteTexture(SDL_GPUTexture* texture)
{
	SDL_ReleaseGPUTexture(dev, texture);
}

TextureManager::~TextureManager()
{
    preview.Destroy(dev);

    for (auto& pending : texture_trash) {
        if (pending.tex) SDL_ReleaseGPUTexture(dev, pending.tex);
    }
    texture_trash.clear();
}

