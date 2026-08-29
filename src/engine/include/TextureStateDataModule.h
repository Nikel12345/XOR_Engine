#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class BufferManager;
class ObjectManager;
class MaterialManager;
struct SceneData;
struct UploadTask;

// Состояния переключаемых вариантов текстур → РАЗРЕЖЕННЫЙ канал из трёх буферов:
//   DEFAULT_TEX_STATE_RANK_BUFFER  — uint2 на СЛОВО из 32 строк: x = биты присутствия,
//                                    y = число носителей ДО этого слова (пословный
//                                    префикс-popcount). Полноразмерный, но 2 бита на строку;
//   DEFAULT_TEX_STATE_INDEX_BUFFER — uint на НОСИТЕЛЯ: смещение его ячеек в state. Компактный,
//                                    адресуется rank'ом;
//   DEFAULT_TEX_STATE_BUFFER       — uint на ячейку, элементы подряд; внутри элемента по секции
//                                    на материал сущности, ровно MAX_VARIATIVE_SLOTS ячеек.
//
// Механизм общий с разреженным текст-каналом UI (UI_DataModule: rank/index/text): CPU-половина —
// SparseRankChannel.h, шейдерная — shaders_code/sparse_rank.hlsli (макрос SPARSE_CHANNEL).
// Переключает варианты малая доля сущностей, поэтому полноразмерным остаётся только presence-бит,
// а всё остальное живёт по числу НОСИТЕЛЕЙ. Прежняя раскладка (int-смещение на строку) тратила
// 32 бита на строку ради значения, которое почти везде значит «ничего нет»: на 800k это 3.05 МиБ
// против 200 КБ теперь.
//
// ОДИН модуль на три буфера (ограничение «инструкция пишет один буфер» реально, «модуль
// обслуживает один буфер» нет: LightDataModule держит два, UI_DataModule три). Держать их
// вместе обязательно: rank даёт номер носителя, index по этому номеру — смещение, по которому
// StoreState положила ячейки. Согласовать три проекции может только один и тот же обход в одном
// и том же порядке; разведи по модулям — разъезд даст чужие варианты, без краша и без лога.
//
// Кто попадает в канал, решает ТЕГ TextureStateComponent, а не содержимое states. Из этого
// следуют РАЗНЫЕ режимы у буферов:
//
//   RANK и INDEX гейтятся ревизией батчей послотно (как BoundSphereDataModule над тем же доменом
//   строк). Они зависят только от порядка строк, наличия тега и числа материалов сущности — всё
//   это меняется структурно, а структуру ревизия батчей и отслеживает. От НОМЕРА варианта не
//   зависят вовсе, поэтому переключение их не трогает и гейт ничего не пропускает. Массив по
//   слотам, а не bool: буферы Dynamic, то есть BUFFERING_LEVEL независимых GPU-буферов, и
//   пропущенная заливка оставляет в слоте то, что писали в ЭТОТ слот в прошлый раз, — bool'а
//   хватило бы на один кадр из трёх, остальные два показывали бы прежнее.
//
//   STATE льётся КАЖДЫЙ КАДР без гейта, и это дёшево: домен фильтруется тегом на входе в ForEach.
//   Гейта тут и не может быть — смена номера варианта структуру не меняет намеренно (в этом вся
//   идея фичи), и ни один структурный сигнал её не видит.
class TextureStateDataModule {
public:
	TextureStateDataModule();

	// СТРОИТ канал: домен строк (тот же и в том же порядке, что у TransformDataModule/
	// InstanceDataModule — rank адресуется строкой, разъезд = чужие варианты) плюс список
	// носителей. Обязана идти ПЕРВОЙ из трёх инструкций: CalculateIndexSize читает её результат.
	uint32_t CalculateRankSize(ObjectManager* om, SceneData* scene, uint64_t revision, uint8_t slot);
	void     StoreRank(BufferManager* bm, UploadTask* task);

	// Читают готовое: обход уже сделан в CalculateRankSize этого же кадра. Движок гоняет ВСЕ
	// size_fn и только потом все updater'ы — это порядок ФАЗ; порядок РЕГИСТРАЦИИ значим лишь
	// между size_fn'ами, а три инструкции регистрируются подряд в SetDefaultTexStateUpdaters.
	uint32_t CalculateIndexSize(uint64_t revision, uint8_t slot);
	void     StoreIndex(BufferManager* bm, UploadTask* task);

	// Size-фазе MaterialManager НЕ нужен: длина элемента = materials.size() * MAX_VARIATIVE_SLOTS,
	// а это данные компонента — резолвить материалы незачем.
	uint32_t CalculateStateSize(ObjectManager* om, SceneData* scene);
	// mtm — резолвер имени материала, ПАРАМЕТРОМ (полем не хранится, см. CLAUDE.md).
	void     StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene,
	                    MaterialManager* mtm);

private:
	// Носители: строка и смещение её ячеек, по ВОЗРАСТАНИЮ строки. Это не CPU-отражение буфера,
	// а его компактный источник — длина по числу переключающихся, а не по домену (в этом весь
	// смысл разреженного канала). index заливается прямо отсюда, rank раскладывает это по словам.
	std::vector<uint32_t> hit_rows_;
	std::vector<uint32_t> hit_ofs_;
	uint32_t rows_ = 0;

	uint64_t last_rank_revision[BUFFERING_LEVEL];
	uint64_t last_index_revision[BUFFERING_LEVEL];
};
