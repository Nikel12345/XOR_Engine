#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <functional>   // LoadSceneTextures: колбэк создания из файла (декод — верхний слой)
#include "config.h"
#include "TransferManager.h"
#include "TextureData.h"

struct UploadTaskTexture {
	SDL_GPUTextureRegion dst{};
	std::vector<std::byte> pixels;          // транзитные пиксели этой задачи (с gutter'ом, если pad>0)
	std::string name;                       // для диагностики при упаковке
	TextureHandle* target_handle = nullptr; // куда писать UVL + через него атлас для GenerateMipmaps
	Uint32 offset = 0;
	Uint32 size = 0;
	Uint32 width = 0, height = 0, pitch = 0;
	bool placed = false;                    // уже размещена в атласе (защита от повторной вставки)

};

// Персистентное состояние упаковщика атласа (свободные места по слоям). Живёт МЕЖДУ вызовами
// PackAtlases, поэтому CreateTexture после загрузки садится в оставшееся место, а не пересобирает
// атлас; DeleteTexture возвращает регион в это состояние для переиспользования. Определён в .cpp
// (использует типы rectpack2D) — здесь только forward-declaration.
struct AtlasPacker;

namespace DefaultSamplersNames {
	inline constexpr const char* DEFAULT_SAMPLER = "_DefaultSampler";
	inline constexpr const char* DEFAULT_SHADOW_SAMPLER = "_DefaultShadowSampler";
	inline constexpr const char* VSM_SAMPLER = "_VsmSampler";
	inline constexpr const char* ENV_SAMPLER = "_EnvSampler";
};

struct PendingTextureDestroy {
	SDL_GPUTexture* tex;
	uint64_t ready_at = 0;   // стамп освобождения, семантика — как у PendingDestroy (BufferManager.h)
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
	// �������� TextureAtlas �� ��� ������������ TextureAtlas
	// Create TextureAtlas from an already existing TextureAtlas
	// ������������ ��� �������� ������ � ������ ���������. �� ������ ����� GPU ��������, ���������� ��������� �� �������� � existing_atlas
	TextureAtlas* CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler);
	// Регистрирует текстуру из уже декодированных пикселей (BGRA32, width*height*4).
	// Загрузку с диска делает TextureLoader; оркестрация — в EngineContext.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);

	//// �������� ������ TextureData � ��������� �����������, ��� �������� ������ � ��������
	//// Create an empty TextureData with specified parameters, without uploading data to the texture
	//TextureData* CreateTextureData(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler);

	//// �������� TextureData �� ��� ������������ SDL_GPUTexture
	//// Create TextureData from an already existing SDL_GPUTexture
	//TextureData* CreateTextureData(const std::string& name, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

	// �������� ������ GPU ��������
	SDL_GPUTexture* CreateGPU_Texture(SDL_GPUTextureCreateInfo tci);

	void GenerateMipmaps(SDL_GPUCommandBuffer* cb);

	// Арендует transfer-буфер у TransferManager и возвращает его; владелец fence фазы
	// обязан вернуть его через TransferManager::ReleaseTB ПОСЛЕ ожидания fence.
	// nullptr (нет задач) — допустим, ReleaseTB(nullptr) — no-op.
	TransferBufferData* ExecuteUploadTasks(SDL_GPUCopyPass* cp);
	// CPU-упаковка атласов (rectpack) → присваивает текстурам UVL. ДОЛЖНА вызываться ДО
	// сборки батчей: батч копирует UVL ЗНАЧЕНИЯМИ (см. TextureBatchData), не указателями,
	// поэтому к моменту BuildRenderBatches UVL уже обязан быть посчитан. Детерминирована и
	// идемпотентна (тот же набор задач → те же UVL). GPU-загрузка пикселей — отдельно, в
	// ExecuteUploadTasks (ей нужен copy-pass), упаковке же GPU не нужен.
	void PackAtlases() { _BuildUploadTasks(); }
	SDL_GPUSampler* CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci);
	SDL_GPUSampler* GetSampler(const std::string& name);
	
	// Немедленное освобождение GPU-текстуры (как QueueDeleteTexture, но без отложенной очереди).
	void DeleteTexture(SDL_GPUTexture* texture);

	// Удаление ОДНОЙ текстуры из атласа: снимает хэндл и освобождает его регион (GPU-текстуру
	// атласа НЕ трогает — атлас общий). Слой удалённой текстуры пересобирается «начисто» (весь слой
	// снова один прямоугольник минус выжившие), поэтому освободившееся место сливается в крупный
	// остаток. Выжившие не двигаются.
	void DeleteTextureHandle(const std::string& name);

	// Merge-upsert текстур из манифеста сцены (см. SceneTextureEntry): занятое имя снимается
	// (replace, как UpsertTexture), затем create_from_file — декод файла остаётся верхнему слою
	// (EngineContext::CreateTextureFromFile), TM владеет только словарной семантикой. Ресурсы,
	// которых нет в манифесте, НЕ трогаются (кодовая инфраструктура переживает загрузку).
	// Возвращает число успешно созданных.
	size_t LoadSceneTextures(const std::vector<SceneTextureEntry>& entries,
		const std::function<TextureHandle*(const SceneTextureEntry&)>& create_from_file);

	SharedDepthTarget* CreateSharedDepthTarget(SDL_GPUTextureCreateInfo tci);
	void QueueDeleteTexture(SDL_GPUTexture* texture);
	void TrashTextures(uint64_t fences_done);

	// ── Превью-атлас UI: общий 2D (атласы движка — 2D_ARRAY, ImGui сэмплит только texture2D).
	//    Сетка фиксированных ячеек PREVIEW_CELL px; регион текстуры блитится в её ячейку
	//    (SDL_BlitGPUTexture, LINEAR) на upload-cb сразу после GenerateMipmaps — тот же поток
	//    (sim/prepare) и fence-цикл, очередь одно-поточная, без замков. ──
	static constexpr uint32_t PREVIEW_ATLAS_SIZE = 2048;
	static constexpr uint32_t PREVIEW_CELL = 64;                                    // 16×16 = 256 превью
	static constexpr uint32_t PREVIEW_PER_ROW = PREVIEW_ATLAS_SIZE / PREVIEW_CELL;
	// Дренаж дёрти-превью: блит региона атласа (слой+UVL) в ячейку. Зовётся с upload-cb.
	void BlitPendingPreviews(SDL_GPUCommandBuffer* cb);
	SDL_GPUTexture* GetPreviewAtlasTexture() const { return preview_atlas; }
	// UV ячейки превью хэндла в превью-атласе (для ImGui::Image/AddImage). valid=false — превью нет.
	struct PreviewUV { bool valid = false; float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };
	PreviewUV GetPreviewUV(const TextureHandle* h) const;

	~TextureManager();

	// Разделяемый depth основного прохода (MAIN/TRANSPARENT/DEBUG). Ресайзится через ->Resize().
	SharedDepthTarget* main_pass_depth = nullptr;

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
	std::vector<UploadTaskTexture> upload_tasks;

	// Превью-атлас UI (см. публичный блок): владение здесь, ячейки — счётчик + фрилист.
	// preview_dirty пушится в _PlaceTask (UVL готов) и дренится BlitPendingPreviews —
	// оба на sim/prepare, одно-поточно.
	void QueuePreviewBlit(TextureHandle* h);
	SDL_GPUTexture* preview_atlas = nullptr;
	std::vector<TextureHandle*> preview_dirty;
	std::vector<int32_t> preview_free_cells;
	int32_t preview_next_cell = 0;

	std::vector<std::unique_ptr<SharedDepthTarget>> shared_depth_targets;
	// Очередь ЦЕЛИКОМ владеется render-потоком: оба пуша (ResizeSceneHDRTargets / Resize depth —
	// по размеру свопчейна) и дренаж (TrashTextures в RenderFunc) живут в нём. Без замков —
	// симметрично трэшам буферов/пайплайнов, которыми так же монопольно владеет sim.
	std::deque<PendingTextureDestroy> texture_trash;

	SDL_GPUDevice* dev = nullptr;
	TransferManager* trm = nullptr;
};

