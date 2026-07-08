#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <functional>
#include "PositionStructure.h"
#include "ModelData.h"

class BufferManager;
struct UploadTask;

// Генератор геометрии процедурной модели: заполняет переданные массивы вершин/индексов
// как угодно — от руками записанного квада до математической поверхности. MM это безразлично.
using ModelGeneratorFn = std::function<void(std::vector<PosUVNormal>&, std::vector<Uint32>&)>;

class ModelManager
{
public:
	ModelManager();
	// Жадно читает модель с диска: заголовок, вершины/индексы в staging и submeshes —
	// всё сразу, так что ресурс пригоден уже в init. Отложена только заливка на GPU
	// (staging копится до батч-апдейтера). CreateModel всегда на prep-потоке.
	// Reads the model from disk eagerly: header, vertices/indices into staging and
	// submeshes — all up front, so the resource is usable right away in init. Only the
	// GPU upload is deferred (staging accumulates until the batch updater).
	ModelData* CreateModel(const std::string& name, const std::string& path, const std::string& path_ind, AnchorShift anchor = AnchorShift::Keep);

	// Процедурная модель: геометрию выдаёт generator (вызывается жадно, как и чтение с диска).
	ModelData* CreateModel(const std::string& name, ModelGeneratorFn generator, AnchorShift anchor = AnchorShift::Keep);

	uint32_t CalculateModelsVerticesSize();
	uint32_t CalculateModelsIndicesSize();

	// Базовое смещение (в байтах) для дозаписи в конец GPU-буфера — append-точка.
	// Менеджер сам знает конец своих буферов; это число прокидывается в offset_fn,
	// чтобы EnsureBufferCapacity учёл его при расчёте ёмкости.
	// Base offset (in bytes) for appending to the end of the GPU buffer — the append point.
	uint32_t GetVertexBaseOffset() const { return gpu_vertices_bytes; }
	uint32_t GetIndexBaseOffset()  const { return gpu_indices_bytes; }

	void UploadModelVertexBuffer(BufferManager* bm, UploadTask* task);
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task);
	bool CheckDirty() const { return dirty; };
	bool CheckDirtySpheres() const { return dirty_spheres; };
	void CommitSpheres() { dirty_spheres = false; };
	ModelData* operator[](const std::string& name);
	// Имя→модель (для UI-браузера ассетов: перечисление плиток). Владение не отдаём.
	const std::unordered_map<std::string, std::unique_ptr<ModelData>>& GetModels() const { return models_data; }
	~ModelManager();

private:
	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	// CPU-буферы накопления: данные моделей, ещё не залитые на GPU. Очищаются после заливки.
	std::vector<PosUVNormal>      staging_vertices;
	std::vector<Uint32>           staging_indices;

	bool dirty = false;
	bool dirty_spheres = true;

	// Счётчики, растущие за каждую загрузку модели (а не размер CPU-буфера).
	uint32_t total_vertices_count = 0;   // всего зарегистрировано вершин (элементы) — для submesh offset
	uint32_t total_indices_count = 0;    // всего зарегистрировано индексов (элементы)
	uint32_t gpu_vertices_bytes = 0;     // уже залито на GPU (байты) — база append для вершин
	uint32_t gpu_indices_bytes = 0;      // уже залито на GPU (байты) — база append для индексов
};