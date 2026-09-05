#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <cstddef>
#include <functional>
#include "PositionStructure.h"
#include "ModelData.h"

class BufferManager;
struct UploadTask;
// ModelGeneratorFn — теперь в ModelData.h (приходит транзитивно): фасады объявляют
// CreateModel(generator) без завязки на этот заголовок.

// Запись манифеста моделей сцены (models.json): рецепт пересоздания из файла. Парсит/пишет json
// верхний слой (Engine::Save/LoadScene); MM сам читает .bin, поэтому колбэк не нужен (в отличие
// от текстур, чей декод живёт в верхнем слое).
struct SceneModelEntry {
	std::string name;
	std::string vertex_path;
	std::string index_path;
	AnchorShift anchor = AnchorShift::Keep;
	std::string pool;   // имя пула геометрии; пусто = дефолтный (старые сцены не мигрируются)
};

class ModelManager
{
public:
	ModelManager();

	// ── Пулы геометрии ─────────────────────────────────────────────────────────────────────
	// Разворачивает раскладку в рабочий пул: генерит имена стрим-буферов, РЕГИСТРИРУЕТ их и
	// индексный буфер в BufferManager и вешает инструкции заливки — сначала ВСЕ стрим-инструкции
	// пула, следом его индексную. Порядок тут структурный, а не по договорённости: индексная
	// финализирует цикл дозагрузки (двигает счётчики и чистит стейджинг), поэтому обязана идти
	// последней; регистрируя обе группы одним вызовом, переставить их нельзя.
	// VRAM при этом не занимается: CreateBufferData только регистрирует обёртку, а BakePending
	// пропускает буфер с usage==0 — пул оживает, когда его стримы назовёт вершинный шейдер.
	// Зовётся с prep-потока на инициализации, ДО создания шейдеров. Пул не удаляется никогда.
	// bm — параметром (менеджеры не владеют друг другом, см. CLAUDE.md); фасад — EngineContext.
	GeometryPool* CreateGeometryPool(BufferManager* bm, const std::string& name, uint32_t vertex_size,
	                                 const std::vector<GeometryPool::StreamDesc>& streams);
	// Промах логируется и даёт nullptr (как GetBufferData). Пустое имя = дефолтный пул.
	GeometryPool* GetPool(const std::string& name);
	// Первый созданный — им пользуются модели и шейдеры, не указавшие пул явно.
	GeometryPool* DefaultPool() const { return default_pool; }
	const std::unordered_map<std::string, std::unique_ptr<GeometryPool>>& GetPools() const { return pools; }

	// ── Модели ─────────────────────────────────────────────────────────────────────────────
	// Жадно читает модель с диска: заголовок, вершины/индексы в staging и submeshes —
	// всё сразу, так что ресурс пригоден уже в init. Отложена только заливка на GPU
	// (staging копится до батч-апдейтера). CreateModel всегда на prep-потоке.
	// pool = nullptr → дефолтный.
	ModelData* CreateModel(const std::string& name, const std::string& path, const std::string& path_ind,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	// Процедурная модель: геометрию выдаёт generator (вызывается жадно, как и чтение с диска).
	// Генератор типизирован PosUVNormal, поэтому годится только пулу с такой же раскладкой —
	// проверяется по VertexSize().
	ModelData* CreateModel(const std::string& name, ModelGeneratorFn generator,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	// Upsert из файла (для редактора): существующий перезагружает В ТОТ ЖЕ объект ModelData,
	// сабмеши пересчитываются на заново-аппендженную геометрию. Старая геометрия остаётся в
	// GPU-буфере (reclaim'а нет — приемлемо для редактора). Новое имя — создаёт. Пересборку
	// батчей взводит вызывающий (она же перерезолвит имена моделей у энтити).
	ModelData* LoadModelFromFile(const std::string& name, const std::string& path, const std::string& path_ind,
	                             AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	// Снос модели: из словаря СРАЗУ, место в пуле — в отложенный возврат (одной пачкой в ближайшем
	// ReclaimRanges). Дисциплина ровно как у TextureManager::DeleteTextureHandle — объект уходит
	// немедленно, GPU-место возвращается позже и разом.
	//
	// ⚠️ Батчи держат СЫРОЙ SubMeshData* внутрь ModelData (ModelBatchData::submesh) — благодаря ему
	// же финализатор заливки правит смещения на месте, и уже собранный батч это видит. Значит после
	// сноса вызывающий ОБЯЗАН взвести пересборку батчей (BatchBuilder::SetDirtyBatches) раньше, чем
	// IndirectDataModule::StoreIndirect снова пройдёт по дереву, иначе это чтение освобождённой
	// памяти. LoadScene это уже делает; команда редактора обязана делать так же.
	// Только sim-поток — как и всякая мутация моделей.
	void DeleteModel(const std::string& name);

	// Модели из манифеста сцены (см. SceneModelEntry): снос существующей + создание заново, ровно
	// как у текстур (TextureManager::LoadSceneTextures). MM сам читает .bin — колбэк не нужен.
	// Модели вне манифеста не трогаются (кодовая инфраструктура переживает загрузку).
	// Ссылки на модель — ИМЕНА (ModelComponent, резолв в BatchBuilder), поэтому смена объекта
	// никого не рвёт: сырой ModelData* держит только процедурная геометрия кода, а её в манифесте
	// не бывает по определению. Возвращает число успешно загруженных.
	size_t LoadSceneModels(const std::vector<SceneModelEntry>& entries);

	// ── Заливка (зовут инструкции, зарегистрированные CreateGeometryPool) ───────────────────
	// Размер заливки ОДНОГО стрима пула (staging_count × stride стрима) и её append-точка
	// (уже залито вершин × stride). Страйд приходит от вызывающего (из таблицы стримов пула) —
	// MM знает только элементный счётчик, канон продвижения.
	uint32_t CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t CalculateModelsIndicesSize(const GeometryPool* pool);

	// База заливки = НАЧАЛО ВЫДЕЛЕННОГО пачке диапазона, а не конец занятого: место снятых моделей
	// переиспользуется. Выделение ленивое и общее на кадр (см. _EnsureBatchAllocation), поэтому
	// не const.
	uint32_t GetVertexBaseOffset(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t GetIndexBaseOffset(const GeometryPool* pool);

	// Заливка одного стрима: выдирает из интерлив-стейджинга (раскладка пула) срез
	// [src_offset, src_offset + stream_stride) каждой вершины и пишет плотно в transfer-буфер.
	void UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
	                             uint32_t src_offset, uint32_t stream_stride);
	// Идёт ПОСЛЕДНЕЙ инструкцией своего пула: финализирует цикл дозагрузки.
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task, const GeometryPool* pool);

	// МАССОВОЕ освобождение: возвращает аллокатору всё место, накопленное сносами, одной пачкой.
	// Зовётся раз в кадр в начале PrepareFunc — то есть ДО того, как _EnsureBatchAllocation выберет
	// место под пачку этого кадра; только так перезагруженная модель садится в освободившееся от
	// неё же место в ТОМ ЖЕ кадре (сначала освобождение, потом размещение).
	//
	// Штампа фенса тут нет НАМЕРЕННО — та же дисциплина, что у текстур
	// (TextureManager::_ReleasePendingRegions): освобождается не GPU-ресурс, а РАЗМЕТКА. Худшее,
	// что даёт переиспользование места под кадром в полёте, — один рваный кадр, который гейт эпох
	// и так отбрасывает. Сам GPU-буфер живёт своей жизнью и сносится по фенсу (PendingDestroy).
	void ReclaimRanges();

	bool CheckDirty() const;
	bool CheckDirtySpheres() const { return dirty_spheres; };
	void CommitSpheres() { dirty_spheres = false; };
	ModelData* operator[](const std::string& name);
	// Тихий резолв имени: промах даёт nullptr и НИЧЕГО не логирует — в отличие от operator[].
	// Нужен там, где промах ЗАКОНЕН (имя у энтити может быть ещё не загружено или уже снесено) или
	// част: ModelComponent резолвится на КАЖДУЮ сущность при сборке батчей, и одно битое имя в
	// сцене на 1М объектов дало бы миллион строк лога. Кто ждёт модель наверняка — берёт
	// operator[], он про промах скажет.
	ModelData* FindModel(const std::string& name) const {
		auto it = models_data.find(name);
		return it != models_data.end() ? it->second.get() : nullptr;
	}
	// Имя→модель (для UI-браузера ассетов: перечисление плиток). Владение не отдаём.
	const std::unordered_map<std::string, std::unique_ptr<ModelData>>& GetModels() const { return models_data; }
	~ModelManager();

private:
	// Модель в пачке текущего кадра. Смещения ОТНОСИТЕЛЬНЫ стейджинга: абсолютную базу даст
	// выделение, а оно случается позже загрузки — поэтому сабмеши патчатся в финализаторе.
	struct BatchEntry {
		ModelData* model = nullptr;
		uint32_t vbase = 0, vcount = 0;   // элементы вершин
		uint32_t ibase = 0, icount = 0;   // элементы индексов
	};

	// Снятое место, ждущее возврата аллокатору. Копии Range, а не указатели в ModelData —
	// поэтому саму модель можно снести сразу, не дожидаясь возврата.
	struct PendingFree {
		RangeAllocator::Range verts;
		RangeAllocator::Range index;
	};

	// Состояние дозагрузки ОДНОГО пула. Пер-пульное, а не менеджерское: у каждого пула своя
	// раскладка (значит свой размер CPU-вершины), свои буферы и своё элементное пространство.
	struct PoolResidency {
		// Стейджинг вершин — БАЙТЫ формата загрузки пула (вершина i начинается с i*VertexSize()).
		// Не типизирован: раскладку знает пул, а не менеджер.
		std::vector<std::byte> staging_vertices;
		std::vector<Uint32>    staging_indices;
		bool dirty = false;

		// Разметка буферов пула. Вершинная одна на ВСЕ стримы: они растут в ногу, элемент №N
		// существует в каждом, поэтому и место у них общее (байты стрима = элементы × его stride).
		RangeAllocator verts;
		RangeAllocator index;

		// Пачка кадра. Выделение ОДНО на весь стейджинг, а не на модель: заливка умеет писать
		// только непрерывный диапазон (одна UpdateInstruction = одна задача), поэтому в дыру
		// садится вся пачка целиком либо она уходит в конец. Освобождение при этом остаётся
		// по-модельным, и соседние возвраты сливаются — поэтому перезагрузка той же сцены
		// попадает ровно в освободившееся от неё же место.
		RangeAllocator::Range batch_verts;
		RangeAllocator::Range batch_index;
		bool batch_allocated = false;
		std::vector<BatchEntry> batch;

		std::vector<PendingFree> pending_free;
	};

	// Общая загрузка файловой модели В переданный ptr (читает staging, пересчитывает submeshes,
	// пишет self-describing пути). Разделяют CreateModel(file) и LoadModelFromFile.
	ModelData* _LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert,
	                          const std::string& path_ind, AnchorShift anchor);
	// Пул модели: явный аргумент, иначе дефолтный. nullptr — пулов вообще нет (ошибка вызывающего).
	GeometryPool* _ResolvePool(GeometryPool* pool);
	// Выделение пачки кадра — лениво и один раз. Зовут его и вершинные offset_fn, и индексная;
	// какая доберётся первой, та и выделит. Порядок не гарантирован: стрим бейкается только когда
	// его назвал шейдер, так что при теневом vs вершинных инструкций может отработать всего одна.
	void _EnsureBatchAllocation(const GeometryPool* pool);
	// Место модели — в отложенный возврат (сразу отдать нельзя: слоты в полёте по нему рисуют).
	// Пул берём из самой модели: она могла быть загружена в другой, и вернуть надо туда.
	void _ReleaseModelRanges(ModelData* model);
	// Регистрация модели в пачке кадра: без дубля (перезагрузка того же имени дважды за кадр).
	void _PushBatchEntry(PoolResidency& res, ModelData* model, uint32_t vbase, uint32_t vcount,
	                     uint32_t ibase, uint32_t icount);
	PoolResidency& _Residency(const GeometryPool* pool) { return residency[pool]; }
	const PoolResidency* _FindResidency(const GeometryPool* pool) const;

	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	std::unordered_map<std::string, std::unique_ptr<GeometryPool>> pools;
	std::unordered_map<const GeometryPool*, PoolResidency>         residency;
	GeometryPool* default_pool = nullptr;

	bool dirty_spheres = true;
};
