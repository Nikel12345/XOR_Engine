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

	// Merge-upsert моделей из манифеста сцены (см. SceneModelEntry): каждая запись через
	// LoadModelFromFile (существующая перезагружается в тот же объект, новая создаётся). MM сам
	// читает .bin — колбэк не нужен. Модели вне манифеста не трогаются (кодовая инфраструктура
	// переживает загрузку). Возвращает число успешно загруженных.
	size_t LoadSceneModels(const std::vector<SceneModelEntry>& entries);

	// ── Заливка (зовут инструкции, зарегистрированные CreateGeometryPool) ───────────────────
	// Размер заливки ОДНОГО стрима пула (staging_count × stride стрима) и её append-точка
	// (уже залито вершин × stride). Страйд приходит от вызывающего (из таблицы стримов пула) —
	// MM знает только элементный счётчик, канон продвижения.
	uint32_t CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t CalculateModelsIndicesSize(const GeometryPool* pool);

	uint32_t GetVertexBaseOffset(const GeometryPool* pool, uint32_t stream_stride) const;
	uint32_t GetIndexBaseOffset(const GeometryPool* pool) const;

	// Заливка одного стрима: выдирает из интерлив-стейджинга (раскладка пула) срез
	// [src_offset, src_offset + stream_stride) каждой вершины и пишет плотно в transfer-буфер.
	void UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
	                             uint32_t src_offset, uint32_t stream_stride);
	// Идёт ПОСЛЕДНЕЙ инструкцией своего пула: финализирует цикл дозагрузки.
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task, const GeometryPool* pool);

	bool CheckDirty() const;
	bool CheckDirtySpheres() const { return dirty_spheres; };
	void CommitSpheres() { dirty_spheres = false; };
	ModelData* operator[](const std::string& name);
	// Имя→модель (для UI-браузера ассетов: перечисление плиток). Владение не отдаём.
	const std::unordered_map<std::string, std::unique_ptr<ModelData>>& GetModels() const { return models_data; }
	~ModelManager();

private:
	// Состояние дозагрузки ОДНОГО пула. Пер-пульное, а не менеджерское: у каждого пула своя
	// раскладка (значит свой размер CPU-вершины), свои буферы и своё элементное пространство.
	struct PoolResidency {
		// Стейджинг вершин — БАЙТЫ формата загрузки пула (вершина i начинается с i*VertexSize()).
		// Не типизирован: раскладку знает пул, а не менеджер.
		std::vector<std::byte> staging_vertices;
		std::vector<Uint32>    staging_indices;
		bool dirty = false;

		// Счётчики, растущие за каждую загрузку модели (а не размер CPU-буфера).
		uint32_t total_vertices_count = 0;   // всего зарегистрировано вершин (элементы) — для submesh offset
		uint32_t total_indices_count = 0;    // всего зарегистрировано индексов (элементы)
		// Уже залито на GPU. Вершины — В ЭЛЕМЕНТАХ (канон lockstep-продвижения всех стримов пула:
		// байтовая append-точка стрима = счётчик × его stride); индексы — в байтах, буфер один.
		uint32_t gpu_vertices_count = 0;
		uint32_t gpu_indices_bytes = 0;
	};

	// Общая загрузка файловой модели В переданный ptr (читает staging, пересчитывает submeshes,
	// пишет self-describing пути). Разделяют CreateModel(file) и LoadModelFromFile.
	ModelData* _LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert,
	                          const std::string& path_ind, AnchorShift anchor);
	// Пул модели: явный аргумент, иначе дефолтный. nullptr — пулов вообще нет (ошибка вызывающего).
	GeometryPool* _ResolvePool(GeometryPool* pool);
	PoolResidency& _Residency(const GeometryPool* pool) { return residency[pool]; }
	const PoolResidency* _FindResidency(const GeometryPool* pool) const;

	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	std::unordered_map<std::string, std::unique_ptr<GeometryPool>> pools;
	std::unordered_map<const GeometryPool*, PoolResidency>         residency;
	GeometryPool* default_pool = nullptr;

	bool dirty_spheres = true;
};
