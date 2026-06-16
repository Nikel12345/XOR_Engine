#pragma once
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <string>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "PositionStructure.h"
#include "ModelData.h"

class BufferManager;
struct UploadTask;

class ModelManager
{
public:
	ModelManager();
	// Регистрирует модель: читает только заголовок (сабмеши/смещения), а тяжёлые
	// данные вершин/индексов грузит отложенно — в LoadModels во время prep-фазы.
	// Registers a model: reads only the header (submeshes/offsets); the heavy
	// vertex/index data is loaded lazily by LoadModels during the prep phase.
	ModelData* CreateModel(const std::string& name, const std::string& path, const std::string& path_ind);

	// Дочитывает с диска все отложенные модели во временные CPU-буферы (staging).
	// Вызывается один раз в общей части prep-фазы (Engine::PrepareFunc), до апдейтеров.
	// Если dirty == false — ничего не делает.
	// Reads all pending models from disk into transient CPU staging buffers.
	// Called once in the common prep phase (Engine::PrepareFunc), before the updaters.
	// No-op when dirty == false.
	void LoadModels();

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
	~ModelManager();

private:
	struct PendingModel {
		ModelData* model = nullptr;   // куда дописать сферы сабмешей после чтения вершин
		std::string vert_path;
		std::string ind_path;
	};

	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	// Отложенные на загрузку модели и временные буферы дозагрузки.
	std::vector<PendingModel>     pending_models;
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