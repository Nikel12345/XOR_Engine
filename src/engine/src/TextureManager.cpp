#include "PCH.h"
#include "TextureManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "finders_interface.h"

struct AtlasPacker {
    struct Layer {
        std::vector<rectpack2D::space_rect> free_spaces;   // свободные прямоугольники слоя

        void reset(int w, int h) {
            free_spaces.clear();
            free_spaces.push_back(rectpack2D::rect_xywh(0, 0, w, h));
        }


        std::optional<rectpack2D::rect_xywh> insert(int w, int h) {
            using namespace rectpack2D;
            for (int i = (int)free_spaces.size() - 1; i >= 0; --i) {
                const space_rect candidate = free_spaces[i];
                const auto splits = insert_and_split(rect_wh(w, h), candidate);
                if (!splits) continue;                         // не помещается — ищем дальше
                free_spaces[i] = free_spaces.back();           // удаляем кандидата (swap-with-back)
                free_spaces.pop_back();
                for (int s = 0; s < splits.count; ++s)         // добавляем остатки-сплиты
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
                if (ir <= ix || ib <= iy) { out.push_back(f); continue; }   // не пересекаются
                if (ix > f.x)         out.push_back(rect_xywh(f.x, f.y, ix - f.x, f.h));          // левый (полная высота)
                if (ir < f.x + f.w)   out.push_back(rect_xywh(ir, f.y, f.x + f.w - ir, f.h));      // правый (полная высота)
                if (iy > f.y)         out.push_back(rect_xywh(ix, f.y, ir - ix, iy - f.y));        // верх (средняя колонка)
                if (ib < f.y + f.h)   out.push_back(rect_xywh(ix, ib, ir - ix, f.y + f.h - ib));   // низ (средняя колонка)
            }
            free_spaces.swap(out);
        }
    };
    std::vector<Layer> layers;   // растёт лениво, size() <= atlas->layers
};

TextureManager::TextureManager(SDL_GPUDevice* device, TransferManager* transfer_manager): dev(device), trm(transfer_manager){
    using namespace DefaultSamplersNames;
    CreateSampler(DEFAULT_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::DEFAULT_SAMPLER));
    CreateSampler(DEFAULT_SHADOW_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SHADOW_SAMPLER));
	CreateSampler(VSM_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::VSM_SAMPLER));
	CreateSampler(ENV_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::ENV_SAMPLER));
    CreateSampler("_SimpleSampler", SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SIMPLE_SAMPLER));

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
        TextureAtlas* text_atlas = CreateTextureAtlas(DefaultAtlasNames::TEXT_ATLAS, tci, GetSampler("_SimpleSampler"));
        text_atlas->padding = 0;
    }

    preview.Create(dev);   // подсистема превью ассетов UI (владеет своей GPU-текстурой)
}

TextureAtlas* TextureManager::CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler)
{
	auto it = atlases_data.find(name);
    if (it != atlases_data.end()) {
        SDL_Log("Texture atlas '%s' already exists, returning existing atlas.", name.c_str());
        return it->second.get();
	}
	auto atlas = std::make_unique<TextureAtlas>();

    atlas->tci = tci;
    atlas->debug_name = name;
    atlas->tci.usage &= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (tci.num_levels > 1)
        atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    atlas->texture_binding.sampler = sampler;
    atlas->width = tci.width;
    atlas->height = tci.height;
    atlas->layers = tci.layer_count_or_depth;
    // Рамка (gutter) нужна ТОЛЬКО мипованному атласу: GenerateMipmaps мипует атлас ЦЕЛИКОМ, и на
    // мипе L кромка тайла усредняется с соседями в радиусе ~2^L текселей. P=2px/сторону — и это же
    // ЕДИНСТВЕННЫЙ источник величины и для рамки в _PlaceTask, и для ужатия текстуры в
    // CreateTextureFromFile, поэтому рассинхрона рамки и сжатия нет by design. Немипованный атлас
    // усреднения по атласу не делает → P=0, рамки нет и ужатия нет. Ключевое: текстуры-степени-двойки
    // ужимаются на 2·P в CreateTextureFromFile, поэтому след контент+2·P остаётся ровно степенью
    // двойки и тайлится впритык.
    atlas->padding = (tci.num_levels > 1) ? 16 : 0;
    atlas->mip_levels = tci.num_levels;
    atlas->format = tci.format;
    atlas->texture_type = tci.type;

	TextureAtlas* ptr = atlas.get();
	atlases_data[name] = std::move(atlas);
	pending_atlas_bakes.push_back(ptr);
	return ptr;
}

TextureAtlas* TextureManager::CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler)
{
    if (!existing_atlas) {
        SDL_Log("Invalid existing atlas provided for new atlas '%s'", name.c_str());
        return nullptr;
    }
    auto it = atlases_data.find(name);
    if (it != atlases_data.end()) {
        SDL_Log("Texture atlas '%s' already exists, returning existing atlas.", name.c_str());
        return it->second.get();
    }
    auto atlas = std::make_unique<TextureAtlas>();

    atlas->shares_with = existing_atlas;
    atlas->tci = existing_atlas->tci;
    atlas->debug_name = name;
    atlas->texture_binding.sampler = sampler;
    atlas->width = existing_atlas->width;
    atlas->height = existing_atlas->height;
    atlas->layers = existing_atlas->layers;
    atlas->padding = existing_atlas->padding;
    atlas->mip_levels = existing_atlas->mip_levels;
    atlas->texture_type = existing_atlas->texture_type;
    atlas->format = existing_atlas->format;

    TextureAtlas* ptr = atlas.get();
    atlases_data[name] = std::move(atlas);
    pending_atlas_bakes.push_back(ptr);
	return ptr;
}

// Дренаж отложенных созданий (каждый кадр, начало PrepareFunc). См. TextureManager.h.
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
            SDL_SetGPUTextureName(dev, atlas->texture_binding.texture, atlas->debug_name.c_str()); 
    }
    for (TextureAtlas* atlas : pending_atlas_bakes) {
        if (!atlas || !atlas->shares_with || atlas->texture_binding.texture) continue;
        atlas->texture_binding.texture = atlas->shares_with->texture_binding.texture;
        if (!atlas->texture_binding.texture)
            SDL_Log("TextureManager::BakePending: shared atlas has no source texture.");
    }

    std::erase_if(pending_atlas_bakes, [](TextureAtlas* a) { return !a || a->texture_binding.texture; });
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels)
{
	auto atlas_it = atlases_data.find(atlas_name);
    if (atlas_it == atlases_data.end()) {
        SDL_Log("Texture atlas '%s' not found for texture '%s'", atlas_name.c_str(), name.c_str());
        return nullptr;
	}
	TextureAtlas* atlas = atlas_it->second.get();
	TextureHandle* th = CreateTexture(name, atlas, w, h, std::move(pixels));
	if (th) th->atlas_name = atlas_name;   // самоописание: имя атласа для редактора/сериализации
	return th;
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels)
{
	if (!atlas) {
        SDL_Log("Invalid atlas provided for texture '%s'", name.c_str());
        return nullptr;
	}

	auto it = handles_data.find(name);
    if (it != handles_data.end()) {
        SDL_Log("Texture '%s' already exists, returning existing texture.", name.c_str());
        return it->second.get();
    }

    auto texture_handle = std::make_shared<TextureHandle>();
    texture_handle->atlas = atlas;

    TextureHandle* ptr = texture_handle.get();
    ptr->width = w;
    ptr->height = h;
    handles_data[name] = std::move(texture_handle);
	atlas->textures.push_back(&ptr->texture_data); 


	CreateUploadTask(ptr, w, h, std::move(pixels), name);

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

    SDL_Surface* c = SDL_ConvertSurface(surface, format);   // не трогает вход; c — новая поверхность
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
    atlas->tci    = tci;              // источник истины для будущих пересозданий
    atlas->width  = tci.width;
    atlas->height = tci.height;
    QueueDeleteTexture(old_tex);      // старую — в отложенное удаление (кадры in-flight)
}

static uint32_t PackUnorm16x2(float x, float y) {
    uint16_t lx = static_cast<uint16_t>(SDL_clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f);
    uint16_t ly = static_cast<uint16_t>(SDL_clamp(y, 0.0f, 1.0f) * 65535.0f + 0.5f);
    return static_cast<uint32_t>(lx) | (static_cast<uint32_t>(ly) << 16);
}

void TextureManager::CreateUploadTask(TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name)
{
    UploadTaskTexture task;
    task.name = name;
    task.pixels = std::move(pixels);
    task.target_handle = handle;
    task.width = w;
    task.height = h;
    task.size = (uint32_t)task.pixels.size();
    texture_upload_tasks.push_back(std::move(task));
}

bool TextureManager::_PlaceTask(UploadTaskTexture& task) {
    using namespace rectpack2D;
    if (task.placed) return true;             // идемпотентность: повторный PackAtlases не сдвинет тайл

    TextureAtlas* atlas = task.target_handle->atlas;
    task.atlas = atlas;   // с этого момента задача самодостаточна — см. UploadTaskTexture::atlas
    auto& packer_uptr = atlas_packers[atlas];
    if (!packer_uptr) packer_uptr = std::make_unique<AtlasPacker>();
    AtlasPacker& packer = *packer_uptr;

    const uint32_t w = task.width, h = task.height;   // нативный размер (до gutter'а)
    if (w == 0 || h == 0) { task.placed = true; return true; }  // пустышка — задачу не грузим

    // Рамка (gutter) — ПО-ОСЕВАЯ и завязана на ОДНУ переменную atlas->padding (P): та же P задаёт
    // величину сжатия текстуры в CreateTextureFromFile, поэтому рассинхрона рамки и сжатия нет
    // by design. Правило на ось: размер == размеру атласа → 0 (полнослойная/кубмап-грань: сосед по
    // этой оси не появится, рамка не влезет и не нужна), иначе → P. padX/padY — ЧИСТАЯ функция от
    // (w,h,atlas) и в точности повторяется в _DecodeOuterRect: извлечение восстанавливает ровно тот
    // внешний прямоугольник, что уложен здесь (симметрия → carve при удалении освобождает точно
    // занятое, без over-carve и утечки). Fallback'а на 0 нет — он бы сломал эту симметрию.
    const uint32_t padX = (w >= atlas->width)  ? 0 : atlas->padding;
    const uint32_t padY = (h >= atlas->height) ? 0 : atlas->padding;

    // Ищем ВНЕШНИЙ (с gutter'ом) прямоугольник слой за слоем от 0-го: текстура садится в ПЕРВЫЙ
    // слой, где помещается → ранние слои заполняются максимально, а очередь не «перескакивает» на
    // новый слой из-за одной не влезшей текстуры. Свободные места персистентны, так что новые
    // текстуры (в т.ч. после загрузки) садятся в остаток, а не поверх уже размещённых.
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

    // ВСЕГДА с рамкой (внешний прямоугольник) — симметрично _DecodeOuterRect. Почти-полнослойный
    // тайл (w+2padX > width, но w < width) честно не влезет — при P=2 это лишь w ∈ {width-4..width-1},
    // практически недостижимо; точно-в-размер (w >= width) сядет с padX=0 выше.
    const bool ok = try_place(w + padX * 2, h + padY * 2, /*allow_new_layer=*/true);

    if (!ok) {
        SDL_Log("Failed to pack task '%s' (%ux%u) - atlas full", task.name.c_str(), w, h);
        return false;
    }

    // UVL считаем от ВНУТРЕННЕГО прямоугольника (outer + pad) и ДО расширения пикселей. Позицию
    // отдельно НЕ храним — регион точно восстанавливается из UVL при удалении (см. _DecodeOuterRect).
    TextureData& td = task.target_handle->texture_data;
    float ox = (float)(outer.x + padX) / (float)atlas->width;
    float oy = (float)(outer.y + padY) / (float)atlas->height;
    float sx = (float)w / (float)atlas->width;
    float sy = (float)h / (float)atlas->height;
    td.uv_packed_offset = PackUnorm16x2(ox, oy);
    td.uv_packed_scale  = PackUnorm16x2(sx, sy);
    td.layer = placed_layer;

    // Превью-заявка ПО ИМЕНИ: внутренний регион (без gutter'а) в пикселях — то, что видит материал.
    // UVL уже записан, размещение фиксировано (task.placed ниже), так что регион больше не сдвинется.
    preview.Request(task.name, atlas, outer.x + padX, outer.y + padY, w, h, placed_layer);

    // Заполняем gutter репликацией кромки: грузим (w+2padX)×(h+2padY) вместо w×h. Без этого паддинг
    // остаётся мусором/чёрным, и мип-генерация (она идёт по атласу целиком) подмешивает его в
    // кромку тайла на грубых мипах → тёмная рамка по периметру меша и битая POM-глубина у края.
    // По-осевое: вертикаль реплицируется clamp'ом sy_row (padY), горизонталь — левой/правой кромкой (padX).
    if (padX > 0 || padY > 0) {
        const uint32_t bpp   = (uint32_t)(task.pixels.size() / ((size_t)w * h));
        const uint32_t new_w = w + padX * 2;
        const uint32_t new_h = h + padY * 2;
        std::vector<std::byte> padded((size_t)new_w * new_h * bpp);
        for (uint32_t y = 0; y < new_h; ++y) {
            const uint32_t sy_row = (uint32_t)SDL_clamp((int)y - (int)padY, 0, (int)h - 1);
            const std::byte* srow = task.pixels.data() + (size_t)sy_row * w * bpp;
            std::byte* drow = padded.data() + (size_t)y * new_w * bpp;
            for (uint32_t x = 0; x < padX; ++x)                      // левая кромка
                SDL_memcpy(drow + (size_t)x * bpp, srow, bpp);
            SDL_memcpy(drow + (size_t)padX * bpp, srow, (size_t)w * bpp);   // центр
            for (uint32_t x = 0; x < padX; ++x)                      // правая кромка
                SDL_memcpy(drow + ((size_t)padX + w + x) * bpp, srow + (size_t)(w - 1) * bpp, bpp);
        }
        task.pixels = std::move(padded);
        task.width  = new_w;
        task.height = new_h;
        task.size   = (uint32_t)task.pixels.size();
    }

    // Грузим от угла ВНЕШНЕГО прямоугольника: при pad>0 картинка уже расширена gutter'ом до
    // (w+2p)×(h+2p), при pad==0 внутренний прямоугольник совпадает с внешним.
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

// Распаковать ВНЕШНИЙ (с gutter'ом) прямоугольник размещения из UVL. unorm16 при размере атласа
// ≤ ~4096 даёт пиксель-в-пиксель (шаг unorm ≫ 1 текселя), поэтому отдельно позицию не храним.
// padX/padY восстанавливаем ТОЙ ЖЕ по-осевой формулой, что и при укладке (_PlaceTask) — чистая
// функция от (w,h,atlas), поэтому декод даёт РОВНО уложенный внешний прямоугольник (без over-/under-
// оценки): carve при удалении освобождает точно занятое место.
static rectpack2D::rect_xywh _DecodeOuterRect(const TextureData& td, const TextureAtlas* atlas) {
    // Нет UVL — текстура ещё НЕ размещалась (создана после прошлого PackAtlases), места в слое не
    // занимает: honest zero. Иначе нулевой внутренний прямоугольник раздулся бы gutter'ом до
    // (0,0,P,P) — фантомный тайл в углу слоя 0, который вырезает у него место навсегда. Для
    // мипованного КУБА (P=16, слой = ровно одна грань) это ломало перезагрузку сцены: грань не
    // влезала в свой слой, все шесть съезжали на +1, шестая не помещалась вовсе.
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
    // Массовое освобождение — зеркало массового размещения ниже, и обязано идти ПЕРЕД ним: только
    // так новые текстуры этого же кадра садятся в место, освободившееся от снятых (перезагрузка
    // сцены — это delete+create одних и тех же имён). Пара (атлас, слой) в списке одна, сколько бы
    // текстур из слоя ни сняли, — в этом весь смысл переноса: пересборка слоя одна на пачку.
    for (const auto& [atlas, layer] : pending_region_release_) {
        auto pit = atlas_packers.find(atlas);
        if (pit == atlas_packers.end() || !pit->second) continue;   // в атласе ничего не размещали
        auto& layers = pit->second->layers;
        if (layer >= layers.size()) continue;

        // Пересборка «начисто»: слой снова один целый прямоугольник минус выжившие. Так место
        // снятых сливается в крупный остаток, а не остаётся набором дыр по их форме. Выжившие
        // не двигаются — их регионы точно восстанавливаются из UVL (см. _DecodeOuterRect).
        auto& lp = layers[layer];
        lp.reset((int)atlas->width, (int)atlas->height);
        for (TextureData* s : atlas->textures)
            if (s->layer == layer) {
                rectpack2D::rect_xywh o = _DecodeOuterRect(*s, atlas);
                if (o.w > 0 && o.h > 0) lp.carve(o);   // ещё не размещённые дают нулевой прямоугольник
            }
    }
    pending_region_release_.clear();
}

void TextureManager::_BuildUploadTasks() {
    // Каждую новую задачу вставляем в ПЕРСИСТЕНТНЫЙ упаковщик её атласа. Уже размещённые задачи
    // (placed) пропускаются, поэтому существующие тайлы не двигаются, а новые садятся в остаток —
    // ни пересборки атласа, ни наложения на старое содержимое.
    for (auto& task : texture_upload_tasks)
        _PlaceTask(task);

    // Размер задачи ДОЛЖЕН совпадать с тем, сколько байт ждёт формат назначения: сюда приходят
    // два разных пути (CreateTexture с BGRA от TextureLoader и FontManager с R8, собранным руками
    // из альфы), и расхождение форматов даст не ошибку валидации, а тихую порчу — сдвиг строк или
    // чтение за границей TB. Ловим здесь, а не по цвету на экране. Размеры уже с gutter'ом:
    // _PlaceTask обновляет width/height/size вместе.
    // Оффсеты в transfer-буфере здесь НЕ считаются: их назначает ExecuteUploadTasks, потому что
    // она может забрать несколько пачек разом (кадры со skip'ом) — нумеровать их по-пачечно
    // значило бы наложить задачи разных пачек друг на друга в одном TB.
    for (auto& t : texture_upload_tasks) {
        const TextureAtlas* a = t.atlas;
        if (!a) continue;
        const uint32_t expect = SDL_CalculateGPUTextureFormatSize(a->format, t.width, t.height, 1);
        if (t.size != expect)
            SDL_Log("Upload '%s': %u байт, а формат атласа '%s' ждёт %u (%ux%u)",
                t.name.c_str(), t.size, a->debug_name.c_str(), expect, t.width, t.height);
    }
}

TransferBufferData* TextureManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp) {
    if (texture_upload_tasks.empty())
        return nullptr;


    constexpr SDL_GPUTextureUsageFlags kMipUsage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // Раскладка задач в transfer-буфере. Оффсет обязан быть кратен размеру текселя формата
    // НАЗНАЧЕНИЯ (VUID-vkCmdCopyBufferToImage-dstImage-07975), а задачи РАЗНЫХ форматов лежат в
    // ОДНОМ буфере: глифы шрифта грузятся в R8-атлас (__TextAtlas) задачами размером w*h — сплошь
    // и рядом нечётным, — и первая же такая задача сбивает выравнивание ВСЕМ следующим за ней
    // 4-байтовым атласам (albedo/normal/env_skybox). Копия с невыровненного оффсета — UB: один
    // драйвер её вытягивает, другой читает со сдвигом на байт, и каналы уезжают (альфа 255
    // попадает в синий → вся текстурированная картинка в «синем фильтре»). Шаг берём у САМОГО
    // формата (SDL знает: 1 у R8, 4 у BGRA8, 8/16 у 16F/32F и BC), а не константой сверху — тогда
    // R8-глифы пакуются впритык, а платят выравниванием только те, кому оно правда нужно.
    // Считаем ЗДЕСЬ, а не на упаковке: в векторе могли накопиться задачи нескольких кадров
    // (прошлая аренда TB не удалась), и пер-кадровая нумерация с нуля наложила бы их друг на друга.
    uint32_t off = 0;
    for (auto& t : texture_upload_tasks) {
        // Атлас у размещённой задачи есть всегда; проверка — чтобы оффсеты остались монотонными,
        // а не «залипли» на нуле, если задача не разместилась.
        const uint32_t align = t.atlas ? SDL_GPUTextureFormatTexelBlockSize(t.atlas->format) : 16;
        if (align > 1) off = (off + align - 1) / align * align;
        t.offset = off;
        off += t.size;
    }
    const uint32_t total = off;

    TransferBufferData* tbd = total ? trm->AcquireUploadTB(total) : nullptr;
    if (!tbd) {
        // total == 0 — все задачи пустые, грузить нечего: чистим, иначе они копились бы вечно.
        // Иначе аренда TB реально провалилась (драйверная аллокация или маппинг) — задачи
        // ОСТАВЛЯЕМ на следующий кадр: места в атласе им уже розданы и не вернутся (_PlaceTask
        // идемпотентен по placed), так что потеря пикселей = мусор в этих регионах навсегда.
        if (total == 0) texture_upload_tasks.clear();
        return nullptr;
    }

    // Решение про мипы принимаем ОДИН раз на атлас за пачку: у шрифтового атласа задача на
    // каждый глиф, и без этого промах по usage залил бы лог сотнями одинаковых строк.
    std::unordered_set<SDL_GPUTexture*> mip_decided;

    for (auto& task : texture_upload_tasks) {
        if (!task.dst.texture) continue;   // задача не разместилась (атлас переполнен) — не грузим
        // Пиксели уже декодированы TextureLoader'ом (BGRA32, плотно) — просто копируем.
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tbd->tb;
        src.offset = task.offset;
        src.pixels_per_row = task.width;
        src.rows_per_layer = task.height;

        std::byte* base = static_cast<std::byte*>(tbd->mapped);
        SDL_memcpy(base + task.offset, task.pixels.data(), task.size);

        SDL_UploadToGPUTexture(cp, &src, &task.dst, false);

        // Заявка на мипы — здесь, где известно, что пиксели этого атласа реально поехали.
        const TextureAtlas* a = task.atlas;
        if (!a || a->mip_levels <= 1) continue;                    // мипов нет — генерировать нечего
        if (!mip_decided.insert(task.dst.texture).second) continue; // по этому атласу уже решили

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
    // Заявки ставит ExecuteUploadTasks (там же и вся валидация usage-флагов) — здесь только
    // дренаж. Живое состояние менеджера (атласы, хэндлы) не читается вообще: в mip_tasks лежат
    // готовые хэндлы текстур, поэтому метод можно звать с другого потока, чем тот, что
    // ставил заявки, и переносить между командбуферами независимо от заливки.
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

void TextureManager::DeleteTextureHandle(const std::string& name)
{
    auto it = handles_data.find(name);
    if (it == handles_data.end()) {
        SDL_Log("Texture handle '%s' not found, cannot delete", name.c_str());
        return;
    }
    TextureHandle* handle = it->second.get();
    TextureData* td = &handle->texture_data;

    if (TextureAtlas* atlas = handle->atlas) {
        // Снятие из списка атласа — НЕМЕДЛЕННО и именно здесь: atlas->textures держит TextureData*
        // внутрь хэндла, который умрёт ниже, а отложить это значило бы оставить висячий указатель.
        auto& v = atlas->textures;
        v.erase(std::remove(v.begin(), v.end(), td), v.end());

        // Возврат места упаковщику — отложенно: помечаем слой, пересоберёт его один раз на всю
        // пачку удалений _ReleasePendingRegions (см. pending_region_release_).
        // uv_packed_scale == 0 — текстура ещё не размещалась (создана после прошлого PackAtlases),
        // места в слое не занимает, освобождать нечего.
        if (td->uv_packed_scale != 0) {
            const std::pair<TextureAtlas*, uint32_t> key{ atlas, td->layer };
            if (std::find(pending_region_release_.begin(), pending_region_release_.end(), key)
                == pending_region_release_.end())
                pending_region_release_.push_back(key);
        }
    }

    // Вектор один, поэтому снимаются ВСЕ незалитые задачи хэндла — раньше половину было не
    // достать (уже переданные render'у пачки), и держалось это на том, что место удалённой
    // текстуры достаётся новой задаче ПОЗЖЕ по порядку, а значит её пиксели лягут последними.
    // Порядок по-прежнему такой, просто лишней заливки байт, которые тут же перезапишутся,
    // теперь не происходит вовсе.
    texture_upload_tasks.erase(
        std::remove_if(texture_upload_tasks.begin(), texture_upload_tasks.end(),
                       [handle](const UploadTaskTexture& t) { return t.target_handle == handle; }),
        texture_upload_tasks.end());

    // Превью НЕ трогаем: его подсистема ключуется ИМЕНЕМ, а не хэндлом. При replace (пересоздание
    // того же имени) слот обязан пережить удаление — иначе плитка мигнёт. Реальное удаление
    // освобождает превью отдельным ReleasePreview(name) в вызывающем (DeleteTexture-команда).
    handles_data.erase(it);   // уничтожает TextureHandle вместе с его TextureData (по значению)
}

size_t TextureManager::LoadSceneTextures(const std::vector<SceneTextureEntry>& entries,
    const std::function<TextureHandle*(const SceneTextureEntry&)>& create_from_file)
{
    // Merge-upsert (см. заголовок): семантика UpsertTexture-команды, только пачкой из манифеста.
    size_t created = 0;
    for (const SceneTextureEntry& e : entries) {
        if (e.name.empty() || e.atlas.empty() || e.path.empty()) {
            SDL_Log("LoadSceneTextures: incomplete entry ('%s') - skipped", e.name.c_str());
            continue;
        }
        if (e.cube) {
            // Хэндлы куба — грани name+"_f0".."_f5" (логического имени в реестре нет): сносим их,
            // иначе CreateTexture вернул бы существующие грани без новой заливки (тихий stale).
            for (int f = 0; f < 6; ++f) {
                const std::string face = e.name + "_f" + std::to_string(f);
                if (handles_data.count(face)) DeleteTextureHandle(face);
            }
        }
        else if (handles_data.count(e.name))
            DeleteTextureHandle(e.name);   // replace под тем же именем (материалы перепривяжутся по имени)
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
    preview.Destroy(dev);   // подсистема превью ассетов UI

    // Дочищаем то, что ещё висело в отложенном удалении.
    for (auto& pending : texture_trash) {
        if (pending.tex) SDL_ReleaseGPUTexture(dev, pending.tex);
    }
    texture_trash.clear();
}

