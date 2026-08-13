#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include "config.h"
#include "TransferManager.h"
#include "TextureData.h"
#include "PreviewPacker.h"

struct UploadTaskTexture {
	SDL_GPUTextureRegion dst{};
	std::vector<std::byte> pixels;
	std::string name; 
	TextureHandle* target_handle = nullptr;
	TextureAtlas* atlas = nullptr;
	Uint32 offset = 0;
	Uint32 size = 0;
	Uint32 width = 0, height = 0, pitch = 0;
	bool placed = false;
};


struct AtlasPacker;

namespace DefaultSamplersNames {
	inline constexpr const char* DEFAULT_SAMPLER = "_DefaultSampler";
	inline constexpr const char* DEFAULT_SHADOW_SAMPLER = "_DefaultShadowSampler";
	inline constexpr const char* VSM_SAMPLER = "_VsmSampler";
	inline constexpr const char* ENV_SAMPLER = "_EnvSampler";
};

namespace DefaultAtlasNames {
	inline constexpr const char* TEXT_ATLAS = "__TextAtlas";
};

struct PendingTextureDestroy {
	SDL_GPUTexture* tex;
	uint64_t ready_at = 0;
};

// Запись манифеста текстур сцены (textures.json): чего достаточно для пересоздания из файла.
// Парсит/пишет json верхний слой (Engine::Save/LoadScene) — TM получает уже разобранный список.
struct SceneTextureEntry {
	std::string name;
	std::string atlas;
	std::string path;
	ChannelConvention conv = ChannelConvention::AsIs;
	// Кубмапа-крест: name — логическое имя куба, пересоздание идёт через CreateCubeMapTexture
	// (6 граней name+"_f0".."_f5" в cube-атлас), conv не применяется.
	bool cube = false;
};

class TextureManager
{
public:
	TextureManager(SDL_GPUDevice* device, TransferManager* transfer_manager);

	TextureAtlas* CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler);
	// Create TextureAtlas from an already existing TextureAtlas
	TextureAtlas* CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler);
	// Загрузку с диска делает TextureLoader; оркестрация — в EngineContext.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);

	SDL_GPUTexture* CreateGPU_Texture(SDL_GPUTextureCreateInfo tci);

	static std::vector<std::byte> SurfaceToPixels(SDL_Surface* surface, SDL_PixelFormat format);


	void BakePending();

	void GenerateMipmaps(SDL_GPUCommandBuffer* cb);

	TransferBufferData* ExecuteUploadTasks(SDL_GPUCopyPass* cp);

	void PackAtlases() { _ReleasePendingRegions(); _BuildUploadTasks(); preview.Publish(); }

	SDL_GPUSampler* CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci);
	SDL_GPUSampler* GetSampler(const std::string& name);
	
	// Немедленное освобождение GPU-текстуры (как QueueDeleteTexture, но без отложенной очереди).
	void DeleteTexture(SDL_GPUTexture* texture);

	void DeleteTextureHandle(const std::string& name);

	// Merge-upsert текстур из манифеста сцены (см. SceneTextureEntry): занятое имя снимается
	// (replace, как UpsertTexture), затем create_from_file — декод файла остаётся верхнему слою
	// (EngineContext::CreateTextureFromFile), TM владеет только словарной семантикой. Ресурсы,
	// которых нет в манифесте, НЕ трогаются (кодовая инфраструктура переживает загрузку).
	// Возвращает число успешно созданных.
	size_t LoadSceneTextures(const std::vector<SceneTextureEntry>& entries,
		const std::function<TextureHandle*(const SceneTextureEntry&)>& create_from_file);

	void QueueDeleteTexture(SDL_GPUTexture* texture);
	void TrashTextures(uint64_t fences_done);

	using TextureResizeFunc = std::function<void(TextureManager&, uint32_t w, uint32_t h)>;
	void CreateResizeInstruction(const std::string& texture_name, TextureResizeFunc fn);
	void ExecuteResizeInstructions(uint32_t w, uint32_t h);

	void RecreateAtlasTexture(TextureAtlas* atlas, SDL_GPUTextureCreateInfo tci);

	void BlitPendingPreviews(SDL_GPUCommandBuffer* cb) { preview.Blit(cb); }
	SDL_GPUTexture* GetPreviewAtlasTexture() const { return preview.Texture(); }
	PreviewPacker::UV GetPreviewUV(const std::string& name) const { return preview.GetUV(name); }
	void ReleasePreview(const std::string& name) { preview.Release(name); }

	~TextureManager();

public:
	TextureHandle* GetTextureHandle(const std::string& name) {
		auto it = handles_data.find(name);
		if (it != handles_data.end()) {
			return it->second.get();
		}
		else {
			SDL_Log("Texture '%s' not found", name.c_str());
			return nullptr;
		}
	};
	// Имя→хэндл (для UI-браузера ассетов: перечисление плиток текстур). Владение не отдаём.
	const std::unordered_map<std::string, std::shared_ptr<TextureHandle>>& GetTextureHandles() const { return handles_data; }
	// Имя→атлас (для UI: дропдаун выбора атласа при создании текстуры).
	const std::unordered_map<std::string, std::unique_ptr<TextureAtlas>>& GetAtlases() const { return atlases_data; }
	TextureAtlas* GetTextureAtlas(const std::string& name) {
		auto it = atlases_data.find(name);
		if (it != atlases_data.end()) {
			return it->second.get();
		}
		else {
			SDL_Log("Texture atlas '%s' not found", name.c_str());
			return nullptr;
		}
	};
private:
	void CreateUploadTask(TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name);

	void _ReleasePendingRegions();
	void _BuildUploadTasks();
	// Разместить одну upload-задачу в персистентном упаковщике её атласа (слой за слоем от 0-го,
	// с переиспользованием освобождённых регионов). При успехе пишет UVL/placement в handle,
	// gutter'ит пиксели и заполняет task.dst. См. TextureManager.cpp.
	bool _PlaceTask(UploadTaskTexture& task);
	std::unordered_map<std::string, std::unique_ptr<TextureAtlas>> atlases_data;
	// shared_ptr — владелец хэндла; материалы ссылаются на текстуру ПО ИМЕНИ (не держат указатель).
	// Поэтому DeleteTextureHandle = просто erase: хэндл освобождается, а материалы на следующей
	// сборке батча не найдут имя → подставят dummy (и перепривяжутся, если имя пересоздадут).
	std::unordered_map<std::string, std::shared_ptr<TextureHandle>> handles_data;
	std::unordered_map<std::string, SDL_GPUSampler*> samplers_data;
	std::unordered_map<TextureAtlas*, std::unique_ptr<AtlasPacker>> atlas_packers;  // персистентное состояние упаковки

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

