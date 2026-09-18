#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <cstddef>
#include <deque>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include "config.h"
#include "TransferManager.h"
#include "TextureData.h"
#include "ResourceTags.h"
#include "PreviewPacker.h"
#include "ResourceRegistry.h"

struct UploadTaskTexture {
	SDL_GPUTextureRegion dst{};
	std::vector<std::byte> pixels;
	std::string name; 
	TextureId id;
	TextureHandle* target_handle = nullptr;
	TextureAtlas* atlas = nullptr;
	Uint32 offset = 0;
	Uint32 size = 0;
	Uint32 width = 0, height = 0, pitch = 0;
	// Слои лежат в pixels стопкой, width×height — размер ОДНОГО слоя: копия SDL берёт ровно слой.
	Uint32 layer_span = 1;
	bool placed = false;
};


struct AtlasPacker;

struct TextureCell {
	std::string name;
	std::shared_ptr<TextureHandle> object;
};
struct AtlasCell {
	std::string name;
	std::unique_ptr<TextureAtlas> object;
};

using TextureRegistry = ResourceRegistry<TextureCell, TextureId>;
using AtlasRegistry   = ResourceRegistry<AtlasCell, AtlasId>;

namespace DefaultSamplersNames {
	inline constexpr const char* DEFAULT_SAMPLER = "DefaultSampler";
	inline constexpr const char* DEFAULT_SHADOW_SAMPLER = "DefaultShadowSampler";
	inline constexpr const char* VSM_SAMPLER = "VsmSampler";
	inline constexpr const char* ENV_SAMPLER = "EnvSampler";
	// NEAREST + clamp. Для глубины: билинейная фильтрация усреднила бы значения с РАЗНЫХ
	// поверхностей, а из них потом восстанавливают позицию — получилась бы точка, которой нет.
	inline constexpr const char* SIMPLE_SAMPLER = "SimpleSampler";
};

namespace DefaultAtlasNames {
	inline constexpr const char* TEXT_ATLAS = "TextAtlas";
};

struct PendingTextureDestroy {
	SDL_GPUTexture* tex;
	uint64_t ready_at = 0;
};

struct SceneTextureEntry {
	std::string name;
	std::string atlas;
	std::string path;
	ChannelConvention conv = ChannelConvention::AsIs;
	bool cube = false;
};

class TextureManager
{
public:
	TextureManager(SDL_GPUDevice* device, TransferManager* transfer_manager);

	TextureAtlas* CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler, ResourceTag tags = ResourceTag::None);
	// Вторая обёртка над ЧУЖОЙ GPU-текстурой: своё имя и свой сэмплер, тело забирается у источника
	// на бейке (до этого в источник вливается usage, объявленный этой обёртке).
	// ТОЛЬКО для атласов, которые не ресайзятся: указатель на текстуру копируется один раз, а
	// RecreateAtlasTexture у источника отправит прежнюю в отложенное удаление, и обёртка останется
	// с освобождённой. Вызывающих сейчас нет.
	TextureAtlas* CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler, ResourceTag tags = ResourceTag::None);
	// layer_span > 1 (грани кубмапы): w/h обязаны совпасть с размером слоя, pixels держат слои стопкой.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span = 1, ResourceTag tags = ResourceTag::None);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span = 1, ResourceTag tags = ResourceTag::None);

	SDL_GPUTexture* CreateGPU_Texture(SDL_GPUTextureCreateInfo tci);

	static std::vector<std::byte> SurfaceToPixels(SDL_Surface* surface, SDL_PixelFormat format);


	void BakePending();

	void GenerateMipmaps(SDL_GPUCommandBuffer* cb);

	TransferBufferData* ExecuteUploadTasks(SDL_GPUCopyPass* copy_pass);

	void PackAtlases() { _ReleasePendingRegions(); _BuildUploadTasks(); preview.Publish(); }

	SDL_GPUSampler* CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci);
	SDL_GPUSampler* GetSampler(const std::string& name);
	
	void DeleteTexture(SDL_GPUTexture* texture);

	bool DeleteTextureHandle(TextureId id, NameSlot slot);
	bool RenameTexture(TextureId id, const std::string& new_name);

	size_t LoadSceneTextures(const std::vector<SceneTextureEntry>& entries,
		const std::function<TextureHandle*(const SceneTextureEntry&)>& create_from_file);

	size_t ClearSceneTextures();

	void QueueDeleteTexture(SDL_GPUTexture* texture);
	void TrashTextures(uint64_t fences_done);

	// Спец-логика вывода размера у каждого таргета своя и живёт в ЗАМЫКАНИИ; сюда приходит размер
	// НАЗНАЧЕНИЯ (свопчейн). key — ключ идемпотентной замены, обычно имя текстуры.
	using TextureResizeFunc = std::function<void(TextureManager&, uint32_t new_width, uint32_t new_height)>;
	void CreateResizeInstruction(const std::string& key, TextureResizeFunc fn);
	void ExecuteResizeInstructions(uint32_t new_width, uint32_t new_height);

	void RecreateAtlasTexture(TextureAtlas* atlas, SDL_GPUTextureCreateInfo tci);

	bool IsDirty() const {
		return !texture_upload_tasks.empty() || !mip_tasks.empty() || preview.HasPendingBlits();
	}

	void BlitPendingPreviews(SDL_GPUCommandBuffer* cb) { preview.Blit(cb); }
	SDL_GPUTexture* GetPreviewAtlasTexture() const { return preview.Texture(); }
	PreviewPacker::UV GetPreviewUV(TextureId id) const { return preview.GetUV(id); }
	void ReleasePreview(TextureId id) { preview.Release(id); }

	~TextureManager();

public:
	TextureHandle* GetTextureHandle(TextureId id) const { return handles_data.Get(id); }
	TextureHandle* GetTextureHandle(const std::string& name) const {
		TextureHandle* h = handles_data.Get(handles_data.Find(name));
		if (!h) SDL_Log("Texture '%s' not found", name.c_str());
		return h;
	}
	TextureId          TextureIdOf(const std::string& name) const { return handles_data.Find(name); }
	TextureId          InternTexture(const std::string& name)     { return handles_data.Intern(name); }
	const std::string& TextureNameOf(TextureId id) const          { return handles_data.NameOf(id); }
	const TextureRegistry& Textures() const { return handles_data; }

	TextureAtlas* GetTextureAtlas(AtlasId id) const { return atlases_data.Get(id); }
	TextureAtlas* GetTextureAtlas(const std::string& name) const {
		TextureAtlas* a = atlases_data.Get(atlases_data.Find(name));
		if (!a) SDL_Log("Texture atlas '%s' not found", name.c_str());
		return a;
	}
	AtlasId            AtlasIdOf(const std::string& name) const { return atlases_data.Find(name); }
	AtlasId            InternAtlas(const std::string& name)     { return atlases_data.Intern(name); }
	const std::string& AtlasNameOf(AtlasId id) const            { return atlases_data.NameOf(id); }
	const AtlasRegistry& Atlases() const { return atlases_data; }
private:
	void CreateUploadTask(TextureId id, TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name, uint32_t layer_span);

	void _ReleasePendingRegions();
	void _BuildUploadTasks();
	bool _PlaceTask(UploadTaskTexture& task);
	AtlasRegistry atlases_data;
	TextureRegistry handles_data;
	std::unordered_map<std::string, SDL_GPUSampler*> samplers_data;
	std::unordered_map<TextureAtlas*, std::unique_ptr<AtlasPacker>> atlas_packers;

	std::vector<std::pair<TextureAtlas*, uint32_t>> pending_region_release_;

	std::unordered_set<SDL_GPUTexture*> mip_tasks;
	std::vector<UploadTaskTexture> texture_upload_tasks;

	PreviewPacker preview;

	std::unordered_map<std::string, TextureResizeFunc> resize_instructions_;

	std::deque<PendingTextureDestroy> texture_trash;


	std::vector<TextureAtlas*> pending_atlas_bakes;

	SDL_GPUDevice* dev = nullptr;
	TransferManager* trm = nullptr;
};

