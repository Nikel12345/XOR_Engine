#pragma once
#include <string>

class ObjectManager;
class BufferManager;
class UI_DataModule;
class FontManager;
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
class TextureStateDataModule;
class BatchBuilder;

class EngineContext;

namespace DefaultUpdateSet
{
	void SetDefaultCameraUpdater(EngineContext& ctx);
	void SetDefaultPositionUpdater(EngineContext& ctx, TransformDataModule* tdm);
	void SetDefaultInstanceDataUpdater(EngineContext& ctx, InstanceDataModule* idm);
	void SetDefaultLightUpdater(EngineContext& ctx, LightDataModule* ldm);
	void SetDefaultPositionIndexUpdater(EngineContext& ctx, PIB_DataModule* pib_dm);
	void SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm);
	void SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm);
	void SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm);
	void SetDefaultEntityToCmdUpdater(EngineContext& ctx, PIB_DataModule* pib_dm);
	void SetDefaultOutPibUpdater(EngineContext& ctx, LightDataModule* ldm);
	void SetDefaultTexStateUpdaters(EngineContext& ctx, TextureStateDataModule* tsm);
	void SetUITextUpdaters(EngineContext& ctx, UI_DataModule* uidm, FontManager* fm, const std::string& fontName);
};
