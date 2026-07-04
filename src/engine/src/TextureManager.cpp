#include "PCH.h"
#include "TextureManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "finders_interface.h"

// Персистентный упаковщик атласа. Держим список свободных прямоугольников по слоям МЕЖДУ вызовами
// PackAtlases. rectpack2D::empty_spaces прячет свой список свободных мест (вернуть регион нельзя),
// поэтому строим свой тонкий упаковщик поверх свободной функции insert_and_split — с ним удаление
// может честно вернуть регион в пул.
struct AtlasPacker {
    struct Layer {
        std::vector<rectpack2D::space_rect> free_spaces;   // свободные прямоугольники слоя

        void reset(int w, int h) {
            free_spaces.clear();
            free_spaces.push_back(rectpack2D::rect_xywh(0, 0, w, h));
        }

        // Первое подходящее свободное место (порядок обхода — как в rectpack2D::empty_spaces::insert,
        // ветка без flip: с конца к началу). При успехе разбивает место на остатки и возвращает угол.
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

        // Вычесть ЗАНЯТЫЙ прямоугольник из свободного места (гильотинный разрез без перекрытий):
        // каждый пересечённый свободный прямоугольник заменяется на не-перекрывающиеся куски вокруг
        // занятого. Используется при пересборке слоя после удаления — слой сбрасывается в один целый
        // прямоугольник, затем вычитаются прямоугольники выживших, и остаток остаётся КРУПНЫМ.
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
}

TextureAtlas* TextureManager::CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler)
{
	auto it = atlases_data.find(name);
    if (it != atlases_data.end()) {
        SDL_Log("Texture atlas '%s' already exists, returning existing atlas.", name.c_str());
        return it->second.get();
	}
	auto atlas = std::make_unique<TextureAtlas>();

	SDL_GPUTexture* tex = CreateGPU_Texture(tci);
    if (!tex) {
        SDL_Log("Failed to create GPU texture for atlas '%s': %s", name.c_str(), SDL_GetError());
        return nullptr;
	}
    atlas->texture_binding.texture = tex;
    atlas->texture_binding.sampler = sampler;
    atlas->width = tci.width;
    atlas->height = tci.height;
    atlas->layers = tci.layer_count_or_depth;
    // Мипованный атлас: GenerateMipmaps мипует атлас ЦЕЛИКОМ, на мипе L кромка тайла усредняется
    // с соседями в радиусе ~2^L текселей. 16px паддинга (+заполнение кромкой в _BuildUploadTasks)
    // держат мипы 0..4 чистыми; 3px хватало бы только для мипов 0..1 → тёмная рамка по краю тайла.
    atlas->padding = (tci.num_levels > 1) ? 16 : 3;
    atlas->mip_levels = tci.num_levels;
    atlas->format = tci.format;
    atlas->texture_type = tci.type;

	TextureAtlas* ptr = atlas.get();
	atlases_data[name] = std::move(atlas);
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
    atlas->texture_binding.texture = existing_atlas->texture_binding.texture;
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
	return ptr;
}

TextureHandle* TextureManager::CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels)
{
	auto atlas_it = atlases_data.find(atlas_name);
    if (atlas_it == atlases_data.end()) {
        SDL_Log("Texture atlas '%s' not found for texture '%s'", atlas_name.c_str(), name.c_str());
        return nullptr;
	}
	TextureAtlas* atlas = atlas_it->second.get();
	return CreateTexture(name, atlas, w, h, std::move(pixels));
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
	atlas->textures.push_back(&ptr->texture_data);   // НЕвладеющая ссылка на размещение (владелец — handles_data)

	// Пиксели уходят в транзитную upload-задачу (перемещением); после заливки+мипов они
	// освобождаются в GenerateMipmaps. На CPU ничего не дублируем — источник истины пикселей — GPU.
	CreateUploadTask(ptr, w, h, std::move(pixels), name);

	return ptr;
}

SDL_GPUTexture* TextureManager::CreateGPU_Texture(SDL_GPUTextureCreateInfo tci)
{
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &tci);
    return tex;
}

SharedDepthTarget* TextureManager::CreateSharedDepthTarget(SDL_GPUTextureCreateInfo tci)
{
    auto target = std::make_unique<SharedDepthTarget>();
    target->tci = tci;
    target->owner = this;
    target->texture = CreateGPU_Texture(tci);

    SharedDepthTarget* ptr = target.get();
    shared_depth_targets.push_back(std::move(target));
    return ptr;
}

void TextureManager::QueueDeleteTexture(SDL_GPUTexture* texture)
{
    if (!texture) return;
    texture_trash.push_back({ texture, BUFFERING_LEVEL });
}

void TextureManager::TrashTextures()
{
    auto it = texture_trash.begin();
    while (it != texture_trash.end()) {
        if (it->frame_ready <= 0) {
            SDL_ReleaseGPUTexture(dev, it->tex);
            it = texture_trash.erase(it);
        }
        else {
            it->frame_ready--;
            ++it;
        }
    }
}

void SharedDepthTarget::Resize(uint32_t w, uint32_t h)
{
    if (!owner || w == 0 || h == 0) return;
    if (texture && tci.width == w && tci.height == h) return;   // размер не изменился — нечего пересоздавать

    tci.width = w;
    tci.height = h;
    SDL_GPUTexture* old_texture = texture;
    texture = owner->CreateGPU_Texture(tci);
    owner->QueueDeleteTexture(old_texture);                      // старую — в отложенное удаление
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
    upload_tasks.push_back(std::move(task));
}

bool TextureManager::_PlaceTask(UploadTaskTexture& task) {
    using namespace rectpack2D;
    if (task.placed) return true;             // идемпотентность: повторный PackAtlases не сдвинет тайл

    TextureAtlas* atlas = task.target_handle->atlas;
    auto& packer_uptr = atlas_packers[atlas];
    if (!packer_uptr) packer_uptr = std::make_unique<AtlasPacker>();
    AtlasPacker& packer = *packer_uptr;

    const uint32_t w = task.width, h = task.height;   // нативный размер (до gutter'а)
    if (w == 0 || h == 0) { task.placed = true; return true; }  // пустышка — задачу не грузим

    // Паддинг — всем, КРОМЕ текстур точно в размер атласа (кубмап-грани/полнослойные: паддинг
    // не влезает и не нужен — сосед у них не появится). Раньше pad=0 получал ПЕРВЫЙ тайл слоя —
    // он оставался без gutter'а и ловил mip-bleed по краям, смежным с более поздними тайлами.
    uint32_t pad = (w >= atlas->width || h >= atlas->height) ? 0 : atlas->padding;

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

    bool ok = try_place(w + pad * 2, h + pad * 2, /*allow_new_layer=*/true);

    // Почти-полнослойные: паддинг нигде не влез. Существующие слои всегда непусты (пустые не
    // создаём), значит без паддинга сесть можно только на СВЕЖИЙ слой — пробуем его.
    if (!ok && pad > 0) {
        pad = 0;
        ok = try_place(w, h, /*allow_new_layer=*/true);
    }

    if (!ok) {
        SDL_Log("Failed to pack task '%s' (%ux%u) — atlas full", task.name.c_str(), w, h);
        return false;
    }

    // UVL считаем от ВНУТРЕННЕГО прямоугольника (outer + pad) и ДО расширения пикселей. Позицию
    // отдельно НЕ храним — регион точно восстанавливается из UVL при удалении (см. _DecodeOuterRect).
    TextureData& td = task.target_handle->texture_data;
    float ox = (float)(outer.x + pad) / (float)atlas->width;
    float oy = (float)(outer.y + pad) / (float)atlas->height;
    float sx = (float)w / (float)atlas->width;
    float sy = (float)h / (float)atlas->height;
    td.uv_packed_offset = PackUnorm16x2(ox, oy);
    td.uv_packed_scale  = PackUnorm16x2(sx, sy);
    td.layer = placed_layer;
    td._pad  = 0;

    // Заполняем gutter репликацией кромки: грузим (w+2p)×(h+2p) вместо w×h. Без этого паддинг
    // остаётся мусором/чёрным, и мип-генерация (она идёт по атласу целиком) подмешивает его в
    // кромку тайла на грубых мипах → тёмная рамка по периметру меша и битая POM-глубина у края.
    if (pad > 0) {
        const uint32_t bpp   = (uint32_t)(task.pixels.size() / ((size_t)w * h));
        const uint32_t new_w = w + pad * 2;
        const uint32_t new_h = h + pad * 2;
        std::vector<std::byte> padded((size_t)new_w * new_h * bpp);
        for (uint32_t y = 0; y < new_h; ++y) {
            const uint32_t sy_row = (uint32_t)SDL_clamp((int)y - (int)pad, 0, (int)h - 1);
            const std::byte* srow = task.pixels.data() + (size_t)sy_row * w * bpp;
            std::byte* drow = padded.data() + (size_t)y * new_w * bpp;
            for (uint32_t x = 0; x < pad; ++x)                       // левая кромка
                SDL_memcpy(drow + (size_t)x * bpp, srow, bpp);
            SDL_memcpy(drow + (size_t)pad * bpp, srow, (size_t)w * bpp);   // центр
            for (uint32_t x = 0; x < pad; ++x)                       // правая кромка
                SDL_memcpy(drow + ((size_t)pad + w + x) * bpp, srow + (size_t)(w - 1) * bpp, bpp);
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

void TextureManager::_BuildUploadTasks() {
    // Каждую новую задачу вставляем в ПЕРСИСТЕНТНЫЙ упаковщик её атласа. Уже размещённые задачи
    // (placed) пропускаются, поэтому существующие тайлы не двигаются, а новые садятся в остаток —
    // ни пересборки атласа, ни наложения на старое содержимое.
    for (auto& task : upload_tasks)
        _PlaceTask(task);

    uint32_t off = 0;
    for (auto& t : upload_tasks) {
        t.offset = off;
        off += t.size;
    }
}

TransferBufferData* TextureManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp) {
    if (upload_tasks.empty())
        return nullptr;

    // Упаковка (UVL + task.dst) сделана заранее в PackAtlases() — до сборки батчей,
    // там же назначены offset'ы, поэтому суммарный размер = конец последней задачи.
    const UploadTaskTexture& last = upload_tasks.back();
    TransferBufferData* tbd = trm->AcquireUploadTB(last.offset + last.size);
    if (!tbd)
        return nullptr;

    for (auto& task : upload_tasks) {
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
    };
    return tbd;
}

void TextureManager::GenerateMipmaps(SDL_GPUCommandBuffer* cb)
{
    std::unordered_set<SDL_GPUTexture*> seen;
    for (auto& task : upload_tasks) {
        TextureAtlas* atlas = task.target_handle->atlas;
        SDL_GPUTexture* tex = atlas->texture_binding.texture;
        if (seen.insert(tex).second and atlas->mip_levels > 1){
            SDL_GenerateMipmapsForGPUTexture(cb, tex);
        }
    }
    upload_tasks.clear();
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

// Распаковать ВНЕШНИЙ (с gutter'ом) прямоугольник размещения из UVL. unorm16 при размере атласа
// ≤ ~4096 даёт пиксель-в-пиксель (шаг unorm ≫ 1 текселя), поэтому отдельно позицию не храним.
// pad восстанавливаем той же формулой, что и при укладке; для почти-полнослойных, уложенных без
// паддинга (fallback), формула может дать pad>0 — но тогда получится лишь БОЛЬШИЙ прямоугольник
// (с clamp по границам атласа), т.е. переоценка занятого → безопасно (место не выдастся повторно).
static rectpack2D::rect_xywh _DecodeOuterRect(const TextureData& td, const TextureAtlas* atlas) {
    auto unpack_lo = [](uint32_t p) { return (float)(p & 0xFFFFu) / 65535.0f; };
    auto unpack_hi = [](uint32_t p) { return (float)(p >> 16)      / 65535.0f; };
    const int w  = (int)(unpack_lo(td.uv_packed_scale)  * atlas->width  + 0.5f);
    const int h  = (int)(unpack_hi(td.uv_packed_scale)  * atlas->height + 0.5f);
    const int ix = (int)(unpack_lo(td.uv_packed_offset) * atlas->width  + 0.5f);
    const int iy = (int)(unpack_hi(td.uv_packed_offset) * atlas->height + 0.5f);
    const int pad = ((uint32_t)w >= atlas->width || (uint32_t)h >= atlas->height) ? 0 : atlas->padding;

    const int ox = std::max(0, ix - pad);
    const int oy = std::max(0, iy - pad);
    const int orr = std::min((int)atlas->width,  ix + w + pad);
    const int ob = std::min((int)atlas->height, iy + h + pad);
    return rectpack2D::rect_xywh(ox, oy, orr - ox, ob - oy);
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
        auto& v = atlas->textures;
        v.erase(std::remove(v.begin(), v.end(), td), v.end());

        auto pit = atlas_packers.find(atlas);
        if (pit != atlas_packers.end() && pit->second) {
            const uint32_t L = td->layer;
            if (L < pit->second->layers.size()) {
                auto& layer = pit->second->layers[L];
                layer.reset((int)atlas->width, (int)atlas->height);
                for (TextureData* s : atlas->textures)
                    if (s->layer == L) {
                        rectpack2D::rect_xywh o = _DecodeOuterRect(*s, atlas);
                        if (o.w > 0 && o.h > 0) layer.carve(o);
                    }
            }
        }
    }

    upload_tasks.erase(
        std::remove_if(upload_tasks.begin(), upload_tasks.end(),
                       [handle](const UploadTaskTexture& t) { return t.target_handle == handle; }),
        upload_tasks.end());

    handles_data.erase(it);   // уничтожает TextureHandle вместе с его TextureData (по значению)
}

void TextureManager::DeleteTexture(SDL_GPUTexture* texture)
{
	SDL_ReleaseGPUTexture(dev, texture);
}

TextureManager::~TextureManager()
{
    // Дочищаем то, что ещё висело в отложенном удалении, и текстуры разделяемых depth-таргетов.
    for (auto& pending : texture_trash) {
        if (pending.tex) SDL_ReleaseGPUTexture(dev, pending.tex);
    }
    texture_trash.clear();
    for (auto& target : shared_depth_targets) {
        if (target && target->texture) SDL_ReleaseGPUTexture(dev, target->texture);
    }
    shared_depth_targets.clear();
  //  for (auto& pair : textures_data) {
		//auto& data = pair.second;
  //      if (data->texture.texture) {
  //          SDL_ReleaseGPUTexture(dev, data->texture.texture);
  //      }
  //      if (data->texture.sampler) {
  //          SDL_ReleaseGPUSampler(dev, data->texture.sampler);
		//}
  //  }
  //  textures_data.clear();
}

