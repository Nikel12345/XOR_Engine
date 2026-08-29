#pragma once
#include <cstdint>

class BufferManager;
class ObjectManager;
class MaterialManager;
struct SceneData;
struct UploadTask;

// Состояния переключаемых вариантов текстур → ПАРА буферов:
//   DEFAULT_TEX_STATE_PREFIX_BUFFER — int на СТРОКУ трансформа: где в state начинается элемент
//                                     этой сущности, -1 = элемента нет (ничего не переключено);
//   DEFAULT_TEX_STATE_BUFFER        — uint на ячейку, элементы подряд; внутри элемента по секции
//                                     на материал сущности, ровно MAX_VARIATIVE_SLOTS ячеек.
//
// ОДИН модуль на два буфера (две CreateUpdateInstruction — ограничение «инструкция пишет один
// буфер» реально, «модуль обслуживает один буфер» нет: LightDataModule держит два, UI_DataModule
// четыре). Держать их вместе обязательно: prefix[row] — это ровно то смещение, по которому
// StoreState положила ячейки этой сущности, и согласовать их может только один и тот же обход
// в одном и том же порядке. Разведи по двум модулям — разъезд даст чужие варианты у части
// объектов, без краша и без строки в логе.
//
// Полей НЕТ (как у InstanceDataModule/BoundSphereDataModule): CPU-отражений буферов не держим,
// каждая фаза делает свой обход и пишет по ходу. Промежуточный staging (UI_DataModule) нужен
// там, где буферы взаимозависимы — у текст-канала WordBase это префикс-popcount по Bits; здесь
// префикс и ячейки независимые проекции одного обхода.
//
// Обновляется КАЖДЫЙ КАДР, dirty-гейта нет намеренно: BatchesRevision двигается только на
// структуру дерева батчей, а переключение варианта дерево не трогает (в этом вся идея фичи) —
// гейт по ней пропускал бы ровно те кадры, ради которых всё делается. Чем ловить «состояния
// изменились» — отдельный вопрос; заводить сейчас поле-ревизию значит оставить в коде
// неработающий механизм, который выглядит работающим.
class TextureStateDataModule {
public:
	// Домен — СТРОКИ трансформа, тот же и в том же порядке, что у TransformDataModule/
	// InstanceDataModule (prefix индексируется строкой, разъезд = чужие варианты).
	uint32_t CalculatePrefixSize(ObjectManager* om, SceneData* scene);
	void     StorePrefix(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene);

	// Size-фазе MaterialManager НЕ нужен: длина элемента = materials.size() * MAX_VARIATIVE_SLOTS,
	// а это данные компонента — резолвить материалы незачем.
	uint32_t CalculateStateSize(ObjectManager* om, SceneData* scene);
	// mtm — резолвер имени материала, ПАРАМЕТРОМ (полем не хранится, см. CLAUDE.md).
	void     StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene,
	                    MaterialManager* mtm);

private:
	// Есть ли в сцене хоть одна переключённая сущность. Считает CalculateStateSize (единственный
	// обход, который вынужден трогать MaterialComponent каждой строки), читает StorePrefix —
	// чтобы не платить тот же обход второй раз, а при false не платить его вовсе.
	// Полагаться на «size уже отработал» можно: движок гоняет ВСЕ size_fn, и только потом все
	// updater'ы, — это порядок ФАЗ, а не порядок регистрации инструкций.
	// Это скаляр-вывод обхода, а не CPU-отражение буфера: та же роль, что у
	// BoundSphereDataModule::total_size и PIB_DataModule::total_elements.
	bool any_states = false;
};
