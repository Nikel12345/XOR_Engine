#pragma once
#include <string>

class ObjectManager;
class BufferManager;
class UI_DataModule;
class UI_ClipDataModule;
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
class CullingDataModule;
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
	void SetDefaultCullingUpdaters(EngineContext& ctx, CullingDataModule* cdm);
	// Канал разрежённого ранга: ранг и индекс. Индекс берёт размер из того, что построила size-фаза
	// ранга, поэтому пара регистрируется одним вызовом.
	void SetDefaultTexStateChannel(EngineContext& ctx, TextureStateDataModule* tsm);
	void SetDefaultTexStateUpdater(EngineContext& ctx, TextureStateDataModule* tsm);
	void SetUITextUpdaters(EngineContext& ctx, UI_DataModule* uidm, FontManager* fm, const std::string& fontName);
	void SetUIClipUpdaters(EngineContext& ctx, UI_ClipDataModule* ucdm);
};
