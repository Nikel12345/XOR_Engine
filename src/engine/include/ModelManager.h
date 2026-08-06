#pragma once
#include <vector>
#include <unordered_map>
#include <string>
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
};

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

	// Upsert из файла (для редактора): существующий перезагружает В ТОТ ЖЕ объект ModelData,
	// сабмеши пересчитываются на заново-аппендженную геометрию. Старая геометрия остаётся в
	// GPU-буфере (reclaim'а нет — приемлемо для редактора). Новое имя — создаёт. Пересборку
	// батчей взводит вызывающий (она же перерезолвит имена моделей у энтити).
	ModelData* LoadModelFromFile(const std::string& name, const std::string& path, const std::string& path_ind, AnchorShift anchor = AnchorShift::Keep);

	// Merge-upsert моделей из манифеста сцены (см. SceneModelEntry): каждая запись через
	// LoadModelFromFile (существующая перезагружается в тот же объект, новая создаётся). MM сам
	// читает .bin — колбэк не нужен. Модели вне манифеста не трогаются (кодовая инфраструктура
	// переживает загрузку). Возвращает число успешно загруженных.
	size_t LoadSceneModels(const std::vector<SceneModelEntry>& entries);

	// Размер заливки ОДНОГО стрима пула (staging_count × stride стрима) и её append-точка
	// (уже залито вершин × stride). Страйд приходит от вызывающего (DefaultUpdateSet берёт его
	// из таблицы пула PosUVNormPool) — MM знает только элементный счётчик, канон продвижения.
	uint32_t CalculateModelsVerticesSize(uint32_t stream_stride);
	uint32_t CalculateModelsIndicesSize();

	uint32_t GetVertexBaseOffset(uint32_t stream_stride) const { return gpu_vertices_count * stream_stride; }
	uint32_t GetIndexBaseOffset()  const { return gpu_indices_bytes; }

	// Заливка одного стрима: выдирает из интерлив-стейджинга (PosUVNormal) срез
	// [src_offset, src_offset + stream_stride) каждой вершины и пишет плотно в transfer-буфер.
	// src_offset/stride — из таблицы пула (Stream::src_offset / format->stride).
	void UploadModelVertexStream(BufferManager* bm, UploadTask* task, uint32_t src_offset, uint32_t stream_stride);
	void UploadModelIndexBuffer(BufferManager* bm, UploadTask* task);
	bool CheckDirty() const { return dirty; };
	bool CheckDirtySpheres() const { return dirty_spheres; };
	void CommitSpheres() { dirty_spheres = false; };
	ModelData* operator[](const std::string& name);
	// Имя→модель (для UI-браузера ассетов: перечисление плиток). Владение не отдаём.
	const std::unordered_map<std::string, std::unique_ptr<ModelData>>& GetModels() const { return models_data; }
	~ModelManager();

private:
	// Общая загрузка файловой модели В переданный ptr (читает staging, пересчитывает submeshes,
	// пишет self-describing пути). Разделяют CreateModel(file) и LoadModelFromFile.
	ModelData* _LoadModelFile(ModelData* ptr, const std::string& path_vert, const std::string& path_ind, AnchorShift anchor);

	std::unordered_map<std::string, std::unique_ptr<ModelData>> models_data;

	// CPU-буферы накопления: данные моделей, ещё не залитые на GPU. Очищаются после заливки.
	std::vector<PosUVNormal>      staging_vertices;
	std::vector<Uint32>           staging_indices;

	bool dirty = false;
	bool dirty_spheres = true;

	// Счётчики, растущие за каждую загрузку модели (а не размер CPU-буфера).
	uint32_t total_vertices_count = 0;   // всего зарегистрировано вершин (элементы) — для submesh offset
	uint32_t total_indices_count = 0;    // всего зарегистрировано индексов (элементы)
	// Уже залито на GPU. Вершины — В ЭЛЕМЕНТАХ (канон lockstep-продвижения всех стримов пула:
	// байтовая append-точка стрима = счётчик × его stride); индексы — в байтах, буфер один.
	uint32_t gpu_vertices_count = 0;
	uint32_t gpu_indices_bytes = 0;
};