#pragma once
#include <cstdint>
#include <vector>

class BufferManager;
class ObjectManager;
struct UploadTask;

// ─────────────────────── UI_DataModule (концепт) ───────────────────────
// Разреженный канал UI-текста. Текст есть лишь у малой доли рисуемых энтити, поэтому вместо
// полноразмерного массива {offset,count} на КАЖДЫЙ row трансформа держим четыре буфера:
//
//   rank      — слова канала: бит присутствия на row + пословный префикс-popcount, пара в одном
//               uint2. Полноразмерный, но 2 бита на строку. Механизм общий с каналом состояний
//               вариантов текстур — SparseRankChannel.h / shaders_code/sparse_rank.hlsli.
//   index     — {offset,count} КОМПАКТНО, по одной паре на текстовый элемент. Индекс = rank.
//   text      — коды глифов всех строк подряд; offset/count режут его на срез элемента.
//
// Шейдер по row (из OutPib) берёт rank канала; если он не -1 — читает index[rank] и по нему срез
// text. Так стоимость масштабируется по тексту, а не по всему миру.
//
// РАЗЛОЖЕНО на 3 буфера, но СТРОИТСЯ одним проходом BuildStaging. Обход — через ForEach (как во
// всех дата-модулях): он идёт по архетипам в map-порядке (= порядок render_instance_base) и по
// локальному индексу внутри, то есть по ВОЗРАСТАНИЮ row, поэтому и push_back в index_/text_, и
// список строк-носителей выходят в rank-порядке без сортировки.
//
// КОНЦЕПТ: UpdateInstruction'ы НЕ заведены. Контракт будущей проводки — как у LightDataModule
// (снапшот в size-фазе): BuildStaging(om) зовётся ОДИН раз в size-фазе ПЕРВОГО UI-буфера
// (напр. bits), после чего остальные Calc*Size/Store* лишь читают уже готовый staging. Пример:
//   bm->CreateUpdateInstruction(UI_TEXT_RANK_BUFFER,
//       [uidm](cp,bm,task){ uidm->StoreRank(bm,&task); },
//       [uidm,om]()->uint32_t { uidm->BuildStaging(om); return uidm->CalcRankSize(); });
//   // остальные два: updater = Store{Index,Text}, size_fn = Calc*Size (без Build).
class UI_DataModule {
public:
	UI_DataModule();

	// Один проход по активной сцене: наполняет staging (строки носителей, index, text).
	// Пересобирается КАЖДЫЙ кадр (канал зависит от row трансформа, а строки сдвигаются при
	// create/delete/hide любого drawable — гейт по флагу это не ловит). Пропускает не-UI-текст
	// энтити, поэтому цена — по числу текстовых элементов + O(N/32) на битовую часть. Зовётся в
	// size-фазе первого UI-буфера. Оптимизация на будущее — гейт по ревизии батчей (как PIB).
	void BuildStaging(ObjectManager* om);

	// Размеры staging'а в БАЙТАХ (для size-фаз UpdateInstruction). Значения КЭШИРУЮТСЯ в конце
	// BuildStaging — умножение раз на сборку, а не на каждый вызов. Определения в .cpp.
	uint32_t CalcRankSize()  const;
	uint32_t CalcIndexSize() const;
	uint32_t CalcTextSize()  const;

	// Заливка соответствующего staging-вектора в transfer-буфер таска (одним блобом). Требует
	// уже выполненного BuildStaging этого кадра. Пустой вектор → no-op.
	void StoreRank(BufferManager* bm, UploadTask* task);
	void StoreIndex(BufferManager* bm, UploadTask* task);
	void StoreText(BufferManager* bm, UploadTask* task);

private:
	// staging: capacity переживает кадры (в steady state без аллокаций — как cams в LightDataModule).
	// Строки носителей ПО ВОЗРАСТАНИЮ + размер домена: из них StoreSparseRank раскладывает слова
	// канала на лету, не материализуя полный массив слов на CPU.
	std::vector<uint32_t> hit_rows_;
	uint32_t              rows_ = 0;
	std::vector<uint32_t> index_;      // пары {offset,count} подряд (2 uint на текст-элемент)
	std::vector<uint32_t> text_;       // коды глифов

	// Кэш размеров staging'а в БАЙТАХ (пишутся в конце BuildStaging; index_ = 2 uint на текст-
	// элемент, поэтому это index_.size()*sizeof(uint32_t), а не число пар). Calc*Size их отдают.
	uint32_t rank_size_  = 0;
	uint32_t index_size_ = 0;
	uint32_t text_size_  = 0;
};
