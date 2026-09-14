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
struct PoolResidency;   // состояние дозагрузки одного пула, тело в ModelManager.cpp
struct SceneModelEntry {
	std::string name;
	std::string vertex_path;
	std::string index_path;
	AnchorShift anchor = AnchorShift::Keep;
	std::string pool;   // имя пула геометрии; пусто = дефолтный (старые сцены не мигрируются)
	// Длина списка с числом сабмешей совпадать не обязана: лишнее игнорируется, недостающим
	// остаётся (0,0) = «границ нет».
	std::vector<SubMeshSpan> screen_size_span;
};

class ModelManager
{
public:
	ModelManager();

	// Зовётся с prep-потока на инициализации, ДО создания шейдеров.
	GeometryPool* CreateGeometryPool(BufferManager* bm, const std::string& name, uint32_t vertex_size,
	                                 const std::vector<GeometryPool::StreamDesc>& streams);
	GeometryPool* GetPool(const std::string& name);
	// Первый созданный — им пользуются модели и шейдеры, не указавшие пул явно.
	GeometryPool* DefaultPool() const { return default_pool; }
	const std::unordered_map<std::string, std::unique_ptr<GeometryPool>>& GetPools() const { return pools; }

	// CreateModel всегда на prep-потоке. pool = nullptr → дефолтный.
	ModelData* CreateModel(const std::string& name, const std::string& path, const std::string& path_ind,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	// Генератор типизирован PosUVNormal, поэтому годится только пулу с такой же раскладкой —
	// проверяется по VertexSize().
	ModelData* CreateModel(const std::string& name, ModelGeneratorFn generator,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	ModelData* LoadModelFromFile(const std::string& name, const std::string& path, const std::string& path_ind,
	                             AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);

	// ⚠️ После сноса вызывающий ОБЯЗАН взвести пересборку батчей (BatchBuilder::SetDirtyBatches):
	// батч держит КОПИЮ размещения сабмеша, а освободившееся место ReclaimRanges отдаёт аллокатору
	// уже в ближайшем кадре — узел, доживший до заливки индиректа, нарисует чужую геометрию.
	// LoadScene это уже делает; команда редактора обязана делать так же.
	// Только sim-поток — как и всякая мутация моделей.
	void DeleteModel(const std::string& name);

	size_t LoadSceneModels(const std::vector<SceneModelEntry>& entries);

	uint32_t CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t CalculateModelsIndicesSize(const GeometryPool* pool);

	// Выделение ленивое и общее на кадр (см. _EnsureBatchAllocation), поэтому не const.
	uint32_t GetVertexBaseOffset(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t GetIndexBaseOffset(const GeometryPool* pool);

	void UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
	                             uint32_t src_offset, uint32_t stream_stride);
	// Идёт ПОСЛЕДНЕЙ инструкцией своего пула: финализирует цикл дозагрузки.
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task, const GeometryPool* pool);

	// Обязана идти ДО PackModels того же кадра: сначала освобождение, потом размещение.
	// Фенса не ждёт — освобождается разметка, а не GPU-ресурс (см. док про пулы и frame.md).
	void ReclaimRanges();

	// Размещение пачки кадра: обязано случиться ДО сборки дерева батчей — оно копирует размещение
	// значениями. Идемпотентна: повторный вызов до заливки ничего не сдвинет.
	void PackModels();

	bool CheckDirty() const;
	// Монотонная ревизия ГЕОМЕТРИИ: ++ на каждое (пере)создание модели. Вместе с
	// ObjectManager::EntityRevision гейтит буфер bound-сфер.
	uint64_t SpheresRevision() const { return spheres_revision; };
	// Запись диапазона и бамп ревизии ОДНОЙ операцией: забытый бамп даёт правку, которая молча не
	// доедет до GPU. Запись того же значения — no-op (ползунок редактора дёргает это покадрово).
	void SetSubmeshSpan(const std::string& name, size_t submesh, SubMeshSpan span);
	ModelData* operator[](const std::string& name);
	// Тихий резолв: промах не логируется. Нужен там, где промах законен или част — резолв на
	// каждую сущность при сборке батчей. Кто ждёт модель наверняка, берёт operator[].
	ModelData* FindModel(const std::string& name) const {
		auto it = models_data.find(name);
		return it != models_data.end() ? it->second.get() : nullptr;
	}
	const std::unordered_map<std::string, std::unique_ptr<ModelData>>& GetModels() const { return models_data; }
	~ModelManager();

private:
	ModelData* _LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert,
	                          const std::string& path_ind, AnchorShift anchor);
	// nullptr = пулов вообще нет (ошибка вызывающего).
	GeometryPool* _ResolvePool(GeometryPool* pool);
	// Лениво и один раз: зовут его и вершинные offset_fn, и индексная, какая доберётся первой.
	// Порядок не гарантирован — незабейканный стрим свою инструкцию не выполняет вовсе.
	void _EnsureBatchAllocation(const GeometryPool* pool);
	// Пул берём из самой модели: она могла быть загружена в другой, и вернуть надо туда.
	void _ReleaseModelRanges(ModelData* model);
	// Диапазоны кладутся В МОДЕЛЬ стейджинг-относительными; дубля в списке не заводим.
	void _PushBatchEntry(PoolResidency& res, ModelData* model, GeometryRange verts, GeometryRange index);
	PoolResidency& _Residency(const GeometryPool* pool);
	const PoolResidency* _FindResidency(const GeometryPool* pool) const;

	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	std::unordered_map<std::string, std::unique_ptr<GeometryPool>> pools;
	// Состояние дозагрузки пула — непрозрачное: раскладка места и стейджинг наружу не торчат.
	std::unordered_map<const GeometryPool*, std::unique_ptr<PoolResidency>> residency;
	GeometryPool* default_pool = nullptr;

	uint64_t spheres_revision = 0;   // см. SpheresRevision()
};
