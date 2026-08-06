#pragma once
#include <cstdint>
#include "config.h"

class ObjectManager;
class BufferManager;
class ModelManager;
struct UploadTask;

// Сферы для GPU-каллинга ПО СТРОКАМ ТРАНСФОРМОВ (тот же порядок и отбор архетипов, что
// TransformDataModule: Positions + DrawComponent). На строку — vec4: xyz центр в model-space,
// w радиус (объединение сфер сабмешей модели энтити). Строка без модели получает w = -1 —
// culling-шейдер трактует её как всегда видимую.
// Гейт — ревизия батчей per-slot (создание/удаление энтити двигает строки), как у PIB.
class BoundSphereDataModule {
public:
	BoundSphereDataModule();
	uint32_t CalculateSphereSize(ObjectManager* om, uint64_t revision, uint8_t slot);
	// mm — резолвер имени модели у энтити (ModelComponent хранит имя, не указатель); приходит
	// на вызове, полем не хранится.
	void StoreSpheres(BufferManager* bm, UploadTask* task, ObjectManager* om, ModelManager* mm);
private:
	uint32_t total_size = 0;
	uint64_t last_revision[BUFFERING_LEVEL];
};
