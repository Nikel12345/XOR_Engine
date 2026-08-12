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
#include <mutex>
#include "config.h"
#include "TransferManager.h"
#include "TextureData.h"
#include "PreviewPacker.h"

struct UploadTaskTexture {
	SDL_GPUTextureRegion dst{};
	std::vector<std::byte> pixels;          // транзитные пиксели этой задачи (с gutter'ом, если pad>0)
	std::string name;                       // для диагностики при упаковке
	// ТОЛЬКО для sim-фазы (_PlaceTask пишет сюда UVL, DeleteTextureHandle сравнивает по нему).
	// Разыменовывать его на стадии заливки НЕЛЬЗЯ: хэндлы умирают немедленно в
	// DeleteTextureHandle (handles_data.erase), и задача, уже отданная на исполнение, получила
	// бы висячий указатель.
	TextureHandle* target_handle = nullptr;
	// Атлас назначения — заполняет _PlaceTask. Всё, что нужно знать о приёмнике на стадии
	// заливки (формат для выравнивания, mip_levels и usage для заявки на мипы), берётся отсюда,
	// а не через target_handle. Указатель стабилен: atlases_data только пополняется (оба
	// CreateTextureAtlas* при коллизии имени возвращают существующий), удаления атласов нет.
	TextureAtlas* atlas = nullptr;
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

namespace DefaultAtlasNames {
	// Единый текстовый атлас: глифы ВСЕХ шрифтов пакуются сюда (packer гарантирует уникальные
	// координаты). Создаётся ЗАРАНЕЕ в конструкторе TextureManager (как дефолт-буферы в
	// BufferManager ctor); FontManager в него только пакует, свой атлас не заводит. Ресайза
	// (выделения новой текстуры при переполнении) пока нет — размер фиксированный.
	inline constexpr const char* TEXT_ATLAS = "__TextAtlas";
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
	// Create TextureAtlas from an already existing TextureAtlas
	TextureAtlas* CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler);
	// Регистрирует текстуру из уже декодированных пикселей (BGRA32, width*height*4).
	// Загрузку с диска делает TextureLoader; оркестрация — в EngineContext.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);

	SDL_GPUTexture* CreateGPU_Texture(SDL_GPUTextureCreateInfo tci);

	// Конвертит SDL-поверхность в произвольный пиксельформат и отдаёт ПЛОТНЫЕ пиксели
	// (w*h*bpp, без padding'а строк) — готовые для CreateTexture. Статик/стейтлес: чистое
	// CPU-преобразование, состояние TM не трогает. Входную поверхность не забирает/не разрушает.
	// Пусто при ошибке. bpp берётся из формата (SDL_BYTESPERPIXEL). Общая утилита (FontManager,
	// импорт текстур и т.п.) — чтобы конверсия не переписывалась в каждом вызывающем.
	static std::vector<std::byte> SurfaceToPixels(SDL_Surface* surface, SDL_PixelFormat format);

	// Отложенная инициализация GPU-ресурсов: Create* только РЕГИСТРИРУЮТ обёртку
	// (TextureAtlas) с её tci и кладут в pending; сами SDL-текстуры создаёт этот
	// дренаж — КАЖДЫЙ кадр, в начале Engine::PrepareFunc (до PackAtlases и сборки батчей, которым
	// GPU-текстура уже нужна). Смысл — к моменту создания усвоить все usage-флаги ресурса: игровой
	// апдейт (вместе с объявлением sp/материалов) идёт раньше prepare на том же sim-потоке.
	void BakePending();

	// Дренирует mip_tasks — заявки, поставленные ExecuteUploadTasks для атласов, в которые
	// реально поехали пиксели. Никакого состояния кроме mip_tasks не трогает, upload_tasks
	// больше не читает и не чистит (ими владеет ExecuteUploadTasks).
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
	// Заканчивается ПЕРЕДАЧЕЙ пачки render-потоку (см. _PublishUploadTasks): после неё задачи
	// уходят из-под sim и заливка их больше не касается этого потока.
	// Две фазы по симметрии: сначала МАССОВОЕ освобождение регионов, накопленных удалениями, затем
	// МАССОВОЕ размещение новых задач — иначе новые текстуры не увидели бы освободившееся место.
	void PackAtlases() { _ReleasePendingRegions(); _BuildUploadTasks(); _PublishUploadTasks(); preview.Publish(); }
	SDL_GPUSampler* CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci);
	SDL_GPUSampler* GetSampler(const std::string& name);
	
	// Немедленное освобождение GPU-текстуры (как QueueDeleteTexture, но без отложенной очереди).
	void DeleteTexture(SDL_GPUTexture* texture);

	// Удаление ОДНОЙ текстуры из атласа: снимает хэндл и освобождает его регион (GPU-текстуру
	// атласа НЕ трогает — атлас общий). Хэндл уходит из словаря СРАЗУ, а место возвращается
	// упаковщику ОТЛОЖЕННО — в ближайшем PackAtlases (см. pending_region_release_): слой
	// пересобирается один раз на всю пачку удалений, а не на каждое.
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

	// ── Инструкции ресайза экранных таргетов (FK-паттерн, как UpdateInstruction у буферов, но
	//    СЛОВАРЬ по имени: 1 текстура — 1 функция). Логика ресайза НЕ на struct TextureAtlas (он
	//    остаётся чистым) — ресайзятся лишь те таргеты, кому зарегали инструкцию. Функтор получает
	//    (tm, w, h) и пересоздаёт свой таргет, захватив свою спец-логику в замыкании при регистрации
	//    (напр. i уровня bloom: w>>(1+i)). Исполнять ОБЯЗАН render-поток (владелец трэш-очереди) —
	//    см. Engine::RenderFunc (гейт по изменению размера).
	using TextureResizeFunc = std::function<void(TextureManager&, uint32_t w, uint32_t h)>;
	void CreateResizeInstruction(const std::string& texture_name, TextureResizeFunc fn);
	void ExecuteResizeInstructions(uint32_t w, uint32_t h);
	// Пересоздать GPU-текстуру атласа под новый tci, СОХРАНИВ usage (он из деклараций, не из пресета)
	// и отправив старую в отложенное удаление. Вспомогалка для инструкций ресайза атласов.
	void RecreateAtlasTexture(TextureAtlas* atlas, SDL_GPUTextureCreateInfo tci);

	// ── Превью ассетов UI — самостоятельная подсистема PreviewPacker (ключ = ИМЯ текстуры, не хэндл).
	//    TM только проксирует: запись блитов на render-cb (после GenerateMipmaps — источник обязан
	//    уже содержать пиксели), выдачу UV/текстуры для ImGui и явное освобождение при реальном
	//    удалении. Заявку на превью TM ставит в _PlaceTask (UVL готов), а PackAtlases передаёт
	//    вызревшие заявки render'у (preview.Publish) — как и задачи заливки. ──
	void BlitPendingPreviews(SDL_GPUCommandBuffer* cb) { preview.Blit(cb); }
	SDL_GPUTexture* GetPreviewAtlasTexture() const { return preview.Texture(); }
	// UV ячейки превью ПО ИМЕНИ (для ImGui::Image/AddImage). valid=false — превью для имени нет.
	PreviewPacker::UV GetPreviewUV(const std::string& name) const { return preview.GetUV(name); }
	// Освободить превью-ячейку имени — при РЕАЛЬНОМ удалении/переименовании текстуры (не при replace).
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
	// Массовое освобождение: пересобирает свободное место в слоях, помеченных удалениями
	// (pending_region_release_). Первая фаза PackAtlases — до размещения новых задач.
	void _ReleasePendingRegions();
	void _BuildUploadTasks();
	// Передача накопленной пачки render-потоку. Идиома InputManager: мьютекс держится только на
	// саму передачу, дальше каждая сторона работает со своим вектором без замков.
	void _PublishUploadTasks();
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
	// Слои, ждущие пересборки свободного места после удалений (атлас + номер слоя, без дублей).
	// Хэндл и его запись в atlas->textures снимаются немедленно (иначе висячий TextureData*), а
	// возврат места копится здесь: пачка удалений (загрузка сцены — 18 снятых хэндлов) стоит одну
	// пересборку слоя вместо одной на каждую текстуру, и пересборка идёт по УЖЕ финальному составу
	// выживших, а не N раз по промежуточным. Владелец — sim: и удаления (команды/LoadScene), и
	// дренаж (_ReleasePendingRegions в PackAtlases) идут на нём, поэтому без замков.
	std::vector<std::pair<TextureAtlas*, uint32_t>> pending_region_release_;
	// ── Задачи заливки текстур: производит sim, исполняет render ──
	// upload_tasks — приёмник sim-потока: сюда пишет CreateUploadTask, здесь их размещает в атласе
	// _PlaceTask и отсюда вычищает DeleteTextureHandle. Наружу не видны.
	std::vector<UploadTaskTexture> upload_tasks;
	// Переданные, но ещё не забранные. Единственное, что видят оба потока — и только под мьютексом
	// (по идиоме InputManager::commands_). Пачка НЕ привязана к слоту специально: при frame skip
	// слот перезаписывается, а места в атласе задачам уже розданы безвозвратно, так что потеря
	// пачки означала бы мусор в этих регионах навсегда. Здесь они просто дождутся ближайшего рендера.
	std::mutex upload_handoff_mtx;
	std::vector<UploadTaskTexture> published_upload_tasks;
	// Собственность render-потока: забранное из published_*. Живёт между кадрами — если аренда
	// transfer-буфера не удалась, задачи остаются здесь до следующей попытки.
	std::vector<UploadTaskTexture> render_upload_tasks;
	// Атласы, в которые в этой пачке РЕАЛЬНО поехали пиксели — заявка на мипы. Ставит
	// ExecuteUploadTasks (единственный владелец upload_tasks), дренирует GenerateMipmaps.
	// Раньше GenerateMipmaps сам ходил по upload_tasks и сам же их чистил — то есть мутировал
	// чужое состояние; из-за этого две операции нельзя было развести ни во времени, ни по
	// потокам. Множество, а не вектор: атлас мипуется один раз, сколько бы задач в него ни легло.
	// Хранится ХЭНДЛ ТЕКСТУРЫ, а не атлас: мипуем ровно ту текстуру, в которую писали
	// (task.dst.texture), и на стороне генерации не нужен доступ к живому состоянию менеджера.
	std::unordered_set<SDL_GPUTexture*> mip_tasks;

	// Превью ассетов UI: самодостаточная подсистема (владеет GPU-текстурой + сеткой по имени).
	// Заявку ставит _PlaceTask (UVL готов), дренаж/UV/release — через прокси публичного блока.
	PreviewPacker preview;

	// Инструкции ресайза экранных таргетов (ключ = имя текстуры). Наполняется на init (до старта
	// потоков), исполняется render-потоком (ExecuteResizeInstructions) → мутация только на init, гонок нет.
	std::unordered_map<std::string, TextureResizeFunc> resize_instructions_;

	// Очередь ЦЕЛИКОМ владеется render-потоком: оба пуша (инструкции ресайза таргетов) и дренаж
	// (TrashTextures в RenderFunc) живут в нём. Без замков — симметрично трэшам буферов/пайплайнов,
	// которыми так же монопольно владеет sim.
	std::deque<PendingTextureDestroy> texture_trash;

	// Обёртки, ждущие создания GPU-текстуры (НЕвладеющие — владелец atlases_data). Дренирует
	// BakePending. Атласы-совладельцы (shares_with) создаются после владельцев: источник обязан существовать.
	std::vector<TextureAtlas*> pending_atlas_bakes;

	SDL_GPUDevice* dev = nullptr;
	TransferManager* trm = nullptr;
};

