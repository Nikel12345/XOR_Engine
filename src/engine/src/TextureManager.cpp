#include "PCH.h"
#include "TextureManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "finders_interface.h"

struct AtlasPacker {
    struct Layer {
        std::vector<rectpack2D::space_rect> free_spaces;

        void reset(int w, int h) {
            free_spaces.clear();
            free_spaces.push_back(rectpack2D::rect_xywh(0, 0, w, h));
        }

        // Свободен ЦЕЛИКОМ: пересборка слоя идёт начисто, поэтому «пустой, но раздробленный»
        // слой невозможен, и одного прямоугольника во всю площадь достаточно.
        bool empty(int w, int h) const {
            return free_spaces.size() == 1 && free_spaces[0].x == 0 && free_spaces[0].y == 0
                && free_spaces[0].w == w && free_spaces[0].h == h;
        }


        std::optional<rectpack2D::rect_xywh> insert(int w, int h) {
            using namespace rectpack2D;
            for (int i = (int)free_spaces.size() - 1; i >= 0; --i) {
                const space_rect candidate = free_spaces[i];
                const auto splits = insert_and_split(rect_wh(w, h), candidate);
                if (!splits) continue;
                free_spaces[i] = free_spaces.back();
                free_spaces.pop_back();
                for (int s = 0; s < splits.count; ++s)
                    free_spaces.push_back(splits.spaces[s]);
                return rect_xywh(candidate.x, candidate.y, w, h);
            }
            return std::nullopt;
        }

        void carve(rectpack2D::rect_xywh o) {
            using namespace rectpack2D;
            std::vector<space_rect> out;
            out.reserve(free_spaces.size() + 3);
            for (const space_rect& f : free_spaces) {
                const int ix = std::max(f.x, o.x);
                const int iy = std::max(f.y, o.y);
                const int ir = std::min(f.x + f.w, o.x + o.w);
                const int ib = std::min(f.y + f.h, o.y + o.h);
                if (ir <= ix || ib <= iy) { out.push_back(f); continue; }
                if (ix > f.x)         out.push_back(rect_xywh(f.x, f.y, ix - f.x, f.h));
                if (ir < f.x + f.w)   out.push_back(rect_xywh(ir, f.y, f.x + f.w - ir, f.h));
                if (iy > f.y)         out.push_back(rect_xywh(ix, f.y, ir - ix, iy - f.y));
                if (ib < f.y + f.h)   out.push_back(rect_xywh(ix, ib, ir - ix, f.y + f.h - ib));
            }
            free_spaces.swap(out);
        }
    };
    std::vector<Layer> layers;
};

TextureManager::TextureManager(SDL_GPUDevice* device, TransferManager* transfer_manager): dev(device), trm(transfer_manager){
    using namespace DefaultSamplersNames;
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
    // Одна величина на рамку в _PlaceTask и на ужатие текстуры при импорте — рассинхрона нет.
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

    for (TextureAtlas* atlas : pending_atlas_bakes) {
        if (atlas && atlas->shares_with)
            atlas->shares_with->tci.usage |= atlas->tci.usage;
    }


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

    std::erase_if(pending_atlas_bakes, [](TextureAtlas* a) { return !a || a->texture_binding.texture; });
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span, ResourceTag tags)
{
	const AtlasId aid = atlases_data.Find(atlas_name);
	TextureAtlas* atlas = atlases_data.Get(aid);
    if (!atlas) {
        SDL_Log("Texture atlas '%s' not found for texture '%s'", atlas_name.c_str(), name.c_str());
        return nullptr;
	}
	TextureHandle* th = CreateTexture(name, atlas, w, h, std::move(pixels), layer_span, tags);
	if (th) th->atlas_id = aid;
	return th;
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

    auto texture_handle = std::make_shared<TextureHandle>();
    texture_handle->atlas = atlas;

    TextureHandle* ptr = texture_handle.get();
    ptr->width = w;
    ptr->height = h;
    ptr->tags = tags;
    ptr->texture_data.layer_span = layer_span;
    handles_data.Put(id, std::move(texture_handle));
	atlas->textures.push_back(&ptr->texture_data); 


	CreateUploadTask(id, ptr, w, h, std::move(pixels), name, layer_span);

	return ptr;
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

    SDL_Surface* c = SDL_ConvertSurface(surface, format);
    if (!c) { SDL_Log("TextureManager::SurfaceToPixels: SDL_ConvertSurface failed: %s", SDL_GetError()); return out; }

    const int    w   = c->w, h = c->h;
    const size_t bpp = SDL_BYTESPERPIXEL(format);
    const size_t row = static_cast<size_t>(w) * bpp;
    out.resize(row * h);
    const uint8_t* src = static_cast<const uint8_t*>(c->pixels);
    for (int y = 0; y < h; ++y)   // построчно: pitch поверхности ≥ плотной ширины строки
        SDL_memcpy(out.data() + static_cast<size_t>(y) * row, src + static_cast<size_t>(y) * c->pitch, row);
    SDL_DestroySurface(c);
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
    resize_instructions_[texture_name] = std::move(fn);   // 1 текстура — 1 функция (перезапись)
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

static uint32_t PackUnorm16x2(float x, float y) {
    uint16_t lx = static_cast<uint16_t>(SDL_clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f);
    uint16_t ly = static_cast<uint16_t>(SDL_clamp(y, 0.0f, 1.0f) * 65535.0f + 0.5f);
    return static_cast<uint32_t>(lx) | (static_cast<uint32_t>(ly) << 16);
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
    if (task.placed) return true;             // повторный PackAtlases не сдвинет уложенный тайл

    TextureAtlas* atlas = task.target_handle->atlas;
    task.atlas = atlas;
    auto& packer_uptr = atlas_packers[atlas];
    if (!packer_uptr) packer_uptr = std::make_unique<AtlasPacker>();
    AtlasPacker& packer = *packer_uptr;

    const uint32_t w = task.width, h = task.height;   // нативный размер (до gutter'а)
    if (w == 0 || h == 0) { task.placed = true; return true; }

    // Слой cube-атласа — это ГРАНЬ: одиночный тайл вклеился бы внутрь грани и портил куб молча.
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
        if (w != atlas->width || h != atlas->height) {
            SDL_Log("Task '%s': %u-layer upload must be full-layer sized (%ux%u, atlas layer %ux%u)",
                    task.name.c_str(), span, w, h, atlas->width, atlas->height);
            return false;
        }

        // База кратна span: у cube-array это держит грани одного куба в одном слоте массива.
        uint32_t base = UINT32_MAX;
        for (uint32_t L = 0; L + span <= packer.layers.size(); L += span) {
            bool all_free = true;
            for (uint32_t i = 0; i < span && all_free; ++i)
                all_free = packer.layers[L + i].empty((int)atlas->width, (int)atlas->height);
            if (all_free) { base = L; break; }
        }
        if (base == UINT32_MAX) {
            const uint32_t start = safe_u32((packer.layers.size() + span - 1) / span) * span;
            if (start + span <= atlas->layers) {
                while (packer.layers.size() < start + span) {
                    AtlasPacker::Layer lp;
                    lp.reset((int)atlas->width, (int)atlas->height);
                    packer.layers.push_back(std::move(lp));
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

        TextureData& td = task.target_handle->texture_data;
        td.uv_packed_offset = PackUnorm16x2(0.0f, 0.0f);
        td.uv_packed_scale  = PackUnorm16x2(1.0f, 1.0f);   // != 0 → запись считается размещённой
        td.layer = base;

        preview.Request(task.id, atlas, 0, 0, w, h, base);

        task.dst.texture   = atlas->texture_binding.texture;
        task.dst.x         = 0;
        task.dst.y         = 0;
        task.dst.z         = 0;
        task.dst.layer     = base;
        task.dst.mip_level = 0;
        task.dst.w         = w;
        task.dst.h         = h;
        task.dst.d         = 1;

        task.placed = true;
        return true;
    }

    // padX/padY — чистая функция от (w,h,atlas), повторённая в _DecodeOuterRect: декод обязан
    // дать РОВНО уложенный здесь прямоугольник, иначе освобождение вернёт чужое место.
    const uint32_t padX = (w >= atlas->width)  ? 0 : atlas->padding;
    const uint32_t padY = (h >= atlas->height) ? 0 : atlas->padding;

    uint32_t placed_layer = 0;
    rect_xywh outer{};

    auto try_place = [&](uint32_t ow, uint32_t oh, bool allow_new_layer) -> bool {
        for (uint32_t L = 0; L < packer.layers.size(); ++L)
            if (auto r = packer.layers[L].insert((int)ow, (int)oh)) { placed_layer = L; outer = *r; return true; }
        if (allow_new_layer && packer.layers.size() < atlas->layers) {
            AtlasPacker::Layer lp;
            lp.reset((int)atlas->width, (int)atlas->height);
            if (auto r = lp.insert((int)ow, (int)oh)) {
                packer.layers.push_back(std::move(lp));
                placed_layer = (uint32_t)packer.layers.size() - 1;
                outer = *r;
                return true;
            }
        }
        return false;
    };

    const bool ok = try_place(w + padX * 2, h + padY * 2, /*allow_new_layer=*/true);

    if (!ok) {
        SDL_Log("Failed to pack task '%s' (%ux%u) - atlas full", task.name.c_str(), w, h);
        return false;
    }

    // UVL считаем от ВНУТРЕННЕГО прямоугольника и ДО расширения пикселей.
    TextureData& td = task.target_handle->texture_data;
    float ox = (float)(outer.x + padX) / (float)atlas->width;
    float oy = (float)(outer.y + padY) / (float)atlas->height;
    float sx = (float)w / (float)atlas->width;
    float sy = (float)h / (float)atlas->height;
    td.uv_packed_offset = PackUnorm16x2(ox, oy);
    td.uv_packed_scale  = PackUnorm16x2(sx, sy);
    td.layer = placed_layer;

    preview.Request(task.id, atlas, outer.x + padX, outer.y + padY, w, h, placed_layer);

    // Рамка заполняется репликацией кромки: незаполненную мип-генерация подмешала бы в тайл
    // тёмной каймой по периметру меша.
    if (padX > 0 || padY > 0) {
        const uint32_t bpp   = (uint32_t)(task.pixels.size() / ((size_t)w * h));
        const uint32_t new_w = w + padX * 2;
        const uint32_t new_h = h + padY * 2;
        std::vector<std::byte> padded((size_t)new_w * new_h * bpp);
        for (uint32_t y = 0; y < new_h; ++y) {
            const uint32_t sy_row = (uint32_t)SDL_clamp((int)y - (int)padY, 0, (int)h - 1);
            const std::byte* srow = task.pixels.data() + (size_t)sy_row * w * bpp;
            std::byte* drow = padded.data() + (size_t)y * new_w * bpp;
            for (uint32_t x = 0; x < padX; ++x)
                SDL_memcpy(drow + (size_t)x * bpp, srow, bpp);
            SDL_memcpy(drow + (size_t)padX * bpp, srow, (size_t)w * bpp);
            for (uint32_t x = 0; x < padX; ++x)
                SDL_memcpy(drow + ((size_t)padX + w + x) * bpp, srow + (size_t)(w - 1) * bpp, bpp);
        }
        task.pixels = std::move(padded);
        task.width  = new_w;
        task.height = new_h;
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

static rectpack2D::rect_xywh _DecodeOuterRect(const TextureData& td, const TextureAtlas* atlas) {
    // Ещё не размещённая текстура места не занимает: без этой ветки её нулевой прямоугольник
    // раздулся бы рамкой до (0,0,P,P) и навсегда вырезал место в углу нулевого слоя.
    if (td.uv_packed_scale == 0) return rectpack2D::rect_xywh(0, 0, 0, 0);
    auto unpack_lo = [](uint32_t p) { return (float)(p & 0xFFFFu) / 65535.0f; };
    auto unpack_hi = [](uint32_t p) { return (float)(p >> 16)      / 65535.0f; };
    const int w  = (int)(unpack_lo(td.uv_packed_scale)  * atlas->width  + 0.5f);
    const int h  = (int)(unpack_hi(td.uv_packed_scale)  * atlas->height + 0.5f);
    const int ix = (int)(unpack_lo(td.uv_packed_offset) * atlas->width  + 0.5f);
    const int iy = (int)(unpack_hi(td.uv_packed_offset) * atlas->height + 0.5f);
    const int padX = ((uint32_t)w >= atlas->width)  ? 0 : atlas->padding;
    const int padY = ((uint32_t)h >= atlas->height) ? 0 : atlas->padding;

    const int ox = std::max(0, ix - padX);
    const int oy = std::max(0, iy - padY);
    const int orr = std::min((int)atlas->width,  ix + w + padX);
    const int ob = std::min((int)atlas->height, iy + h + padY);
    return rectpack2D::rect_xywh(ox, oy, orr - ox, ob - oy);
}

void TextureManager::_ReleasePendingRegions() {
    // Обязано идти ПЕРЕД размещением: только так новые текстуры кадра садятся в место, которое
    // освободили снятые.
    for (const auto& [atlas, layer] : pending_region_release_) {
        auto pit = atlas_packers.find(atlas);
        if (pit == atlas_packers.end() || !pit->second) continue;
        auto& layers = pit->second->layers;
        if (layer >= layers.size()) continue;

        auto& lp = layers[layer];
        lp.reset((int)atlas->width, (int)atlas->height);
        // Диапазон, а не равенство: многослойная запись значится только на своём базовом слое, и
        // по равенству остальные её слои стали бы «свободны» под живыми пикселями.
        for (TextureData* s : atlas->textures)
            if (layer >= s->layer && layer < s->layer + s->layer_span) {
                rectpack2D::rect_xywh o = _DecodeOuterRect(*s, atlas);
                if (o.w > 0 && o.h > 0) lp.carve(o);
            }
    }
    pending_region_release_.clear();
}

void TextureManager::_BuildUploadTasks() {
    for (auto& task : texture_upload_tasks)
        _PlaceTask(task);

    // Расхождение размера с форматом назначения даёт не ошибку валидации, а сдвиг строк или
    // чтение за границей TB, поэтому сверяем здесь.
    for (auto& t : texture_upload_tasks) {
        const TextureAtlas* a = t.atlas;
        if (!a) continue;
        const uint32_t expect = SDL_CalculateGPUTextureFormatSize(a->format, t.width, t.height, t.layer_span);
        if (t.size != expect)
            SDL_Log("Upload '%s': %u байт, а формат атласа '%s' ждёт %u (%ux%u, %u слоёв)",
                t.name.c_str(), t.size, a->name.c_str(), expect, t.width, t.height, t.layer_span);
    }
}

TransferBufferData* TextureManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp) {
    if (texture_upload_tasks.empty())
        return nullptr;


    constexpr SDL_GPUTextureUsageFlags kMipUsage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // Смещение задачи обязано быть кратно размеру текселя формата НАЗНАЧЕНИЯ, а в одном буфере
    // едут разные форматы: R8-глиф с нечётным размером сбивает выравнивание всем 4-байтовым
    // атласам за собой, и копия читается со сдвигом на байт. Шаг берём у самого формата, чтобы
    // R8 паковался впритык. Считаем здесь: в векторе могли накопиться задачи нескольких кадров.
    uint32_t off = 0;
    for (auto& t : texture_upload_tasks) {
        const uint32_t align = t.atlas ? SDL_GPUTextureFormatTexelBlockSize(t.atlas->format) : 16;
        if (align > 1) off = (off + align - 1) / align * align;
        t.offset = off;
        off += t.size;
    }
    const uint32_t total = off;

    TransferBufferData* tbd = total ? trm->AcquireUploadTB(total) : nullptr;
    if (!tbd) {
        // Аренда не удалась — задачи ОСТАВЛЯЕМ: места в атласе им уже розданы и не вернутся,
        // так что потерянные пиксели остались бы мусором в этих регионах навсегда.
        if (total == 0) texture_upload_tasks.clear();
        return nullptr;
    }

    // Решение про мипы — одно на атлас за пачку: у шрифтового атласа задача на каждый глиф.
    std::unordered_set<SDL_GPUTexture*> mip_decided;

    for (auto& task : texture_upload_tasks) {
        if (!task.dst.texture) continue;   // не разместилась (атлас переполнен)
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbd->tb;
        src.offset = task.offset;
        src.pixels_per_row = task.width;    // размер ОДНОГО слоя: слои лежат стопкой
        src.rows_per_layer = task.height;

        std::byte* base = static_cast<std::byte*>(tbd->mapped);
        SDL_memcpy(base + task.offset, task.pixels.data(), task.size);

        // Копия — ПО СЛОЮ: многослойного копирования в API нет вовсе.
        const uint32_t span = task.layer_span ? task.layer_span : 1;
        const uint32_t layer_bytes = task.size / span;
        for (uint32_t i = 0; i < span; ++i) {
            SDL_GPUTextureTransferInfo layer_src = src;
            layer_src.offset = task.offset + i * layer_bytes;
            SDL_GPUTextureRegion layer_dst = task.dst;
            layer_dst.layer = task.dst.layer + i;
            SDL_UploadToGPUTexture(cp, &layer_src, &layer_dst, false);
        }

        const TextureAtlas* a = task.atlas;
        if (!a || a->mip_levels <= 1) continue;
        if (!mip_decided.insert(task.dst.texture).second) continue;

        if ((a->tci.usage & kMipUsage) != kMipUsage) {
            SDL_Log("TextureManager::ExecuteUploadTasks: atlas has num_levels=%u but usage lacks "
                    "SAMPLER|COLOR_TARGET - mip generation skipped (would abort on SDL assert).",
                    a->mip_levels);
            continue;
        }
        mip_tasks.insert(task.dst.texture);
    };

    texture_upload_tasks.clear();
    return tbd;
}

void TextureManager::GenerateMipmaps(SDL_GPUCommandBuffer* cb)
{
    // В очереди лежат готовые хэндлы SDL, живого состояния менеджера дренаж не читает — поэтому
    // он не привязан ни к потоку, ни к командбуферу заливки.
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
    TextureData* td = &handle->texture_data;

    if (TextureAtlas* atlas = handle->atlas) {
        // Снятие из списка атласа — НЕМЕДЛЕННО и именно здесь: atlas->textures держит TextureData*
        // внутрь хэндла, который умрёт ниже, а отложить это значило бы оставить висячий указатель.
        auto& v = atlas->textures;
        v.erase(std::remove(v.begin(), v.end(), td), v.end());

        // Возврат места упаковщику отложен: помечаем ВСЕ слои записи, пересборка одна на пачку.
        for (uint32_t i = 0; td->uv_packed_scale != 0 && i < td->layer_span; ++i) {
            const std::pair<TextureAtlas*, uint32_t> key{ atlas, td->layer + i };
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
            DeleteTextureHandle(id, NameSlot::Keep);   // replace в той же ячейке (материалы перепривяжутся по её id)
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

