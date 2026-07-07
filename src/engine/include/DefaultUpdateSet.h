#pragma once

class ObjectManager;
class BufferManager;
class CameraManager;
class Camera;
class PIB_DataModule;
class TransformDataModule;
class InstanceDataModule;
class LightDataModule;
class ModelManager;
class PassManager;
class IndirectDataModule;
class BoundSphereDataModule;
class BatchBuilder;

class EngineContext;

namespace DefaultUpdateSet
{
	void SetDefaultCameraUpdater(EngineContext& ctx);
	void SetDefaultPositionUpdater(EngineContext& ctx, TransformDataModule* tdm);
	void SetDefaultInstanceDataUpdater(EngineContext& ctx, InstanceDataModule* idm);
	void SetDefaultLightUpdater(EngineContext& ctx, LightDataModule* ldm);
	void SetDefaultPositionIndexUpdater(EngineContext& ctx, PIB_DataModule* pib_dm);
	void SetDefaultVertexUpdater(EngineContext& ctx);
	void SetDefaultIndexUpdater(EngineContext& ctx);
	void SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm);
	// Индирект ПО-КАМЕРНЫЙ и per-frame (num_instances=0, scatter дозаполняет атомиком).
	void SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm);
	// GPU-каллинг c компактацией: сферы по строкам + entity->cmd (по ревизии батчей) и
	// ресайз out_pib (пишется scatter-каллингом каждый кадр компактно).
	void SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm);
	void SetDefaultEntityToCmdUpdater(EngineContext& ctx, PIB_DataModule* pib_dm);
	void SetDefaultOutPibUpdater(EngineContext& ctx, LightDataModule* ldm);
};
