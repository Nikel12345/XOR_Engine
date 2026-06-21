#include "PCH.h"
#include "TextureManager.h"
#include "TexturesPresets.h"
#include "TextureSamplerPresets.h"
#include "finders_interface.h"

TextureManager::TextureManager(SDL_GPUDevice* device): ResourceManager(device){
    using namespace DefaultSamplersNames;
    CreateSampler(DEFAULT_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::DEFAULT_SAMPLER));
    CreateSampler(DEFAULT_SHADOW_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SHADOW_SAMPLER));
	CreateSampler(VSM_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::VSM_SAMPLER));
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
    atlas->padding = 3;
    atlas->mip_levels = tci.num_levels;
    atlas->format = tci.format;

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

    auto texture_handle = std::make_unique<TextureHandle>();
    auto td = std::make_unique<TextureData>();

    texture_handle->atlas = atlas;
	texture_handle->texture_data = td.get();

    TextureHandle* ptr = texture_handle.get();
    ptr->width = w;
    ptr->height = h;
    handles_data[name] = std::move(texture_handle);
	atlas->textures_data.push_back(std::move(td));

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

void TextureManager::CreateUploadTask(TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name)
{
    // Размер берём из самих пикселей — TextureLoader уже упаковал их под формат атласа.
    uint32_t size = (uint32_t)pixels.size();

    UploadTaskTexture task;
    task.name = name;
    task.pixels = std::move(pixels);
    task.target_handle = handle;
    task.offset = current_upload_tb_offset;
    task.width = w;
    task.height = h;
	task.size = size;

    current_upload_tb_offset += size;

	upload_tasks.push_back(std::move(task));

}

static uint32_t PackUnorm16x2(float x, float y) {
    uint16_t lx = static_cast<uint16_t>(SDL_clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f);
    uint16_t ly = static_cast<uint16_t>(SDL_clamp(y, 0.0f, 1.0f) * 65535.0f + 0.5f);
    return static_cast<uint32_t>(lx) | (static_cast<uint32_t>(ly) << 16);
}

void TextureManager::_BuildUploadTasks() {
    using namespace rectpack2D;
    using spaces_t = empty_spaces<false>;

    struct AtlasState {
        uint32_t layer = 0;
        uint32_t placed_count = 0;
        std::unique_ptr<spaces_t> spaces;
    };
    std::unordered_map<TextureAtlas*, AtlasState> states;

    for (int idx = 0; idx < (int)upload_tasks.size(); idx++) {
        auto& task = upload_tasks[idx];
        TextureAtlas* atlas = task.target_handle->atlas;
        auto& state = states[atlas];

        if (!state.spaces)
            state.spaces = std::make_unique<spaces_t>(rect_wh((int)atlas->width, (int)atlas->height));

        uint32_t pad = (state.placed_count == 0) ? 0 : atlas->padding;
        int packed_w = (int)(task.width + pad * 2);
        int packed_h = (int)(task.height + pad * 2);

        auto result = state.spaces->insert(rect_wh(packed_w, packed_h));

        if (!result) {
            state.layer++;
            state.placed_count = 0;
            if (state.layer >= atlas->layers) {
                SDL_Log("Atlas out of layers for task '%s'", task.name.c_str());
                continue;
            }
            state.spaces = std::make_unique<spaces_t>(rect_wh((int)atlas->width, (int)atlas->height));

            // На новом слое placed_count == 0, поэтому pad тоже 0
            pad = 0;
            packed_w = (int)task.width;
            packed_h = (int)task.height;
            result = state.spaces->insert(rect_wh(packed_w, packed_h));
        }

        if (!result) {
            SDL_Log("Failed to pack task '%s' even on new layer", task.name.c_str());
            continue;
        }

        task.dst.texture = atlas->texture_binding.texture;
        task.dst.x = (Uint32)result->x + pad;
        task.dst.y = (Uint32)result->y + pad;
        task.dst.z = 0;
        task.dst.layer = state.layer;
        task.dst.mip_level = 0;
        task.dst.w = task.width;
        task.dst.h = task.height;
        task.dst.d = 1;

        TextureData* td = task.target_handle->texture_data;
        float ox = (float)(result->x + pad) / (float)atlas->width;
        float oy = (float)(result->y + pad) / (float)atlas->height;
        float sx = (float)task.width / (float)atlas->width;
        float sy = (float)task.height / (float)atlas->height;
        td->uv_packed_offset = PackUnorm16x2(ox, oy);
        td->uv_packed_scale = PackUnorm16x2(sx, sy);
        td->layer = state.layer;
        td->_pad = 0;

        state.placed_count++;
    }
}

void TextureManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp) {
    if (upload_tasks.empty())
        return;

    if (!mapped_upload_tb) {
        SDL_Log("UploadToTransferBuffer called without mapping the upload transfer buffer");
        return;
    }
    // Упаковка (UVL + task.dst) сделана заранее в PackAtlases() — до сборки батчей.
	EnsureUploadTransferBufferCapacity(current_upload_tb_offset);
    for (auto& task : upload_tasks) {
        // Пиксели уже декодированы TextureLoader'ом (BGRA32, плотно) — просто копируем.
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_transfer_buffer;
        src.offset = task.offset;
        src.pixels_per_row = task.width;
        src.rows_per_layer = task.height;

        std::byte* base = static_cast<std::byte*>(mapped_upload_tb);
        SDL_memcpy(base + task.offset, task.pixels.data(), task.size);

        SDL_UploadToGPUTexture(cp, &src, &task.dst, false);
    };
}

void TextureManager::GenerateMipmaps(SDL_GPUCommandBuffer* cb)
{
    std::unordered_set<SDL_GPUTexture*> seen;
    for (auto& task : upload_tasks) {
        SDL_GPUTexture* tex = task.target_handle->atlas->texture_binding.texture;
        if (seen.insert(tex).second and task.target_handle->atlas->mip_levels > 1){
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

void TextureManager::DeleteTexture(const std::string& name)
{
  //  auto it = textures_data.find(name);
  //  if (it != textures_data.end()) {
  //      SDL_ReleaseGPUTexture(dev, it->second->texture.texture);
		//textures_data.erase(it);
  //  }
  //  else {
  //      SDL_Log("Texture '%s' not found, cannot delete", name.c_str());
  //  }
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

