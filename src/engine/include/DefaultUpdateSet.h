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
	// Стрим- и индексная инструкции геометрии живут не здесь: их вешает
	// ModelManager::CreateGeometryPool вместе с буферами пула — иначе их порядок (все стримы, затем
	// индексная-финализатор) держался бы только на договорённости между двумя вызовами.
	void SetDefaultLightCamerasUpdater(EngineContext& ctx, LightDataModule* ldm);
	// Индирект ПО-КАМЕРНЫЙ и per-frame (num_instances=0, scatter дозаполняет атомиком).
	void SetDefaultIndirectUpdater(EngineContext& ctx, IndirectDataModule* idm, LightDataModule* ldm);
	// GPU-каллинг c компактацией: сферы по строкам + entity->cmd (по ревизии батчей) и
	// ресайз out_pib (пишется scatter-каллингом каждый кадр компактно).
	void SetDefaultBoundSphereUpdater(EngineContext& ctx, BoundSphereDataModule* bdm);
	void SetDefaultEntityToCmdUpdater(EngineContext& ctx, PIB_DataModule* pib_dm);
	void SetDefaultOutPibUpdater(EngineContext& ctx, LightDataModule* ldm);

	// Переключаемые варианты текстур: ПАРА буферов от ОДНОГО модуля (префикс по строкам +
	// плоские ячейки состояний). Порядок регистрации тут безразличен — в отличие от UI-текста,
	// ни один из двух не готовит данные для другого (см. TextureStateDataModule).
	void SetDefaultTexStateUpdaters(EngineContext& ctx, TextureStateDataModule* tsm);

	// UI-текст: 4 буфера разреженного канала (bits/wordbase/index/text из UI_DataModule) +
	// GlyphUVL (из FontManager по имени шрифта). BITS регистрируется первым — его size-фаза
	// строит staging (BuildStaging), остальные лишь читают. Буферы бейкаются, когда их объявит
	// UI-SP (usage) — поэтому SP создаётся ДО первого BakePending (в MainInit).
	void SetUITextUpdaters(EngineContext& ctx, UI_DataModule* uidm, FontManager* fm, const std::string& fontName);
};
