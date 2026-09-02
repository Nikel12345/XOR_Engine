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
	// Сколько ПОДРЯД идущих слоёв заливает задача (dst.layer — первый). pixels держит их стопкой:
	// width×height — размер ОДНОГО слоя, поэтому pixels_per_row/rows_per_layer остаются пер-слойными,
	// а слои развёрстываются шагом size/layer_span. Больше одного слоя за копию SDL не умеет
	// (imageSubresource.layerCount жёстко 1) — заливка идёт циклом, размещение остаётся одним.
	Uint32 layer_span = 1;
	bool placed = false;
};


struct AtlasPacker;

namespace DefaultSamplersNames {
	inline constexpr const char* DEFAULT_SAMPLER = "_DefaultSampler";
	inline constexpr const char* DEFAULT_SHADOW_SAMPLER = "_DefaultShadowSampler";
	inline constexpr const char* VSM_SAMPLER = "_VsmSampler";
	inline constexpr const char* ENV_SAMPLER = "_EnvSampler";
	// NEAREST + clamp. Для глубины: билинейная фильтрация усреднила бы значения с РАЗНЫХ
	// поверхностей, а из них потом восстанавливают позицию — получилась бы точка, которой нет.
	inline constexpr const char* SIMPLE_SAMPLER = "_SimpleSampler";
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
	// Кубмапа-крест 4×3: пересоздание идёт через CreateCubeMapTexture (один хэндл на 6 слоёв
	// cube-атласа), conv не применяется. Для словарной семантики TM ничем не отличается от
	// обычной записи — имя одно.
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
	// layer_span > 1 — одна текстура на НЕСКОЛЬКИХ подряд идущих слоях (грани кубмапы): w/h тогда
	// обязаны совпасть с размером слоя, а pixels держать слои стопкой. Про кубы TM не знает
	// намеренно — знание про них живёт в EngineContext::CreateCubeMapTexture, здесь только слои.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span = 1);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, uint32_t layer_span = 1);

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

	// Есть ли для GPU незаписанная работа: заливки, мипы, блиты превью. Этим гейтится САБМИТ
	// текстурного cb, а не запись в него. Текстурный cb уходит на ГРАФИЧЕСКУЮ очередь (порядок
	// «залили → нарисовали» держит она, а не барьеры), и даже пустой он встаёт в неё ЗА кадром:
	// его фенс отстреливает только когда кадр дорисован. Стадия заливки ждёт оба своих фенса,
	// поэтому пустой сабмит удлиняет оборот слота на пол-кадра и тормозит НЕ рендер, а sim.
	// Три слагаемых, а не одно: превью публикуются независимо от заливок, а мипы могут остаться
	// от заливки, записанной другим cb (Engine_Frame — не единственная реализация кадра).
	bool IsDirty() const {
		return !texture_upload_tasks.empty() || !mip_tasks.empty() || preview.HasPendingBlits();
	}

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
	void CreateUploadTask(TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name, uint32_t layer_span);

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

