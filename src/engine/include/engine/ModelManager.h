#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <cstddef>
#include <functional>
#include "PositionStructure.h"
#include "ModelData.h"
#include "ResourceRegistry.h"

class BufferManager;
struct UploadTask;
struct PoolResidency;

struct SceneModelEntry {
	std::string name;
	std::string vertex_path;
	std::string index_path;
	AnchorShift anchor = AnchorShift::Keep;
	std::string pool; 
	// Длина списка с числом сабмешей совпадать не обязана: лишнее игнорируется, недостающим
	// остаётся (0,0) = «границ нет».
	std::vector<SubMeshSpan> screen_size_span;
};

struct ModelCell { std::string name; std::unique_ptr<ModelData> object; };
using ModelRegistry = ResourceRegistry<ModelCell, ModelId>;

class ModelManager
{
public:
	ModelManager();

	GeometryPool* CreateGeometryPool(BufferManager* bm, const std::string& name, uint32_t vertex_size,
	                                 const std::vector<GeometryPool::StreamDesc>& streams);
	GeometryPool* GetPool(const std::string& name);
	GeometryPool* DefaultPool() const { return default_pool; }
	const std::unordered_map<std::string, std::unique_ptr<GeometryPool>>& GetPools() const { return pools; }

	ModelData* CreateModel(const std::string& name, const std::string& path, const std::string& path_ind,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr, ResourceTag tags = ResourceTag::None);

	// Генератор типизирован PosUVNormal, поэтому годится только пулу с такой же раскладкой —
	// проверяется по VertexSize().
	ModelData* CreateModel(const std::string& name, ModelGeneratorFn generator,
	                       AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr, ResourceTag tags = ResourceTag::None);

	ModelData* LoadModelFromFile(const std::string& name, const std::string& path, const std::string& path_ind,
	                             AnchorShift anchor = AnchorShift::Keep, GeometryPool* pool = nullptr);


	bool DeleteModel(ModelId id, NameSlot slot);
	bool RenameModel(ModelId id, const std::string& new_name);

	size_t LoadSceneModels(const std::vector<SceneModelEntry>& entries);

	size_t ClearSceneModels();

	uint32_t CalculateModelsVerticesSize(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t CalculateModelsIndicesSize(const GeometryPool* pool);

	uint32_t GetVertexBaseOffset(const GeometryPool* pool, uint32_t stream_stride);
	uint32_t GetIndexBaseOffset(const GeometryPool* pool);

	void UploadModelVertexStream(BufferManager* bm, UploadTask* task, const GeometryPool* pool,
	                             uint32_t src_offset, uint32_t stream_stride);
	// Идёт ПОСЛЕДНЕЙ инструкцией своего пула: финализирует цикл дозагрузки.
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task, const GeometryPool* pool);


	void ReclaimRanges();
	void PackModels();

	bool CheckDirty() const;

	uint64_t SpheresRevision() const { return spheres_revision; };
	// Запись диапазона и бамп ревизии ОДНОЙ операцией: забытый бамп даёт правку, которая молча не
	// доедет до GPU. Запись того же значения — no-op (ползунок редактора дёргает это покадрово).
	void SetSubmeshSpan(const std::string& name, size_t submesh, SubMeshSpan span);
	ModelData* operator[](const std::string& name);
	// Тихий резолв: промах не логируется. Нужен там, где промах законен или част — резолв на
	// каждую сущность при сборке батчей. Кто ждёт модель наверняка, берёт operator[].
	ModelData* FindModel(const std::string& name) const { return models_data.Get(models_data.Find(name)); }
	ModelData* FindModel(ModelId id) const              { return models_data.Get(id); }
	ModelId            ModelIdOf(const std::string& name) const { return models_data.Find(name); }
	ModelId            InternModel(const std::string& name)     { return models_data.Intern(name); }
	const std::string& ModelNameOf(ModelId id) const            { return models_data.NameOf(id); }
	const ModelRegistry& Models() const { return models_data; }
	~ModelManager();

private:
	ModelData* _LoadModelFile(ModelData* ptr, GeometryPool* pool, const std::string& path_vert,
	                          const std::string& path_ind, AnchorShift anchor);
	GeometryPool* _ResolvePool(GeometryPool* pool);

	void _EnsureBatchAllocation(const GeometryPool* pool);
	void _ReleaseModelRanges(ModelData* model);
	void _PushBatchEntry(PoolResidency& res, ModelData* model, GeometryRange verts, GeometryRange index);
	PoolResidency& _Residency(const GeometryPool* pool);
	const PoolResidency* _FindResidency(const GeometryPool* pool) const;

	ModelRegistry models_data;
	std::unordered_map<std::string, std::unique_ptr<GeometryPool>> pools;
	std::unordered_map<const GeometryPool*, std::unique_ptr<PoolResidency>> residency;
	GeometryPool* default_pool = nullptr;
	uint64_t spheres_revision = 0;
};
