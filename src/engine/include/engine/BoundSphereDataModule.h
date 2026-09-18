#pragma once
#include <cstdint>
#include "config.h"

class ObjectManager;
class BufferManager;
class ModelManager;
struct UploadTask;

class BoundSphereDataModule {
public:
	BoundSphereDataModule();
	uint32_t CalculateSphereSize(ObjectManager* om, uint64_t revision, uint8_t slot);
	void StoreSpheres(BufferManager* bm, UploadTask* task, ObjectManager* om, ModelManager* mm,
	                  uint64_t revision, uint8_t slot);
private:
	uint32_t total_size = 0;
	uint64_t last_revision[BUFFERING_LEVEL];
};
