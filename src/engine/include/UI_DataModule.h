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
//   bits      — 1 бит присутствия текста на row (по 32 в uint). Полноразмерный, но крохотный.
//   wordbase  — пословный префикс-popcount от bits: rank(row) = wordbase[w] + popcount(биты
//               своего слова ниже позиции). Даёт компактный индекс за O(1) без поиска/хэша.
//   index     — {offset,count} КОМПАКТНО, по одной паре на текстовый элемент. Индекс = rank.
//   text      — коды глифов всех строк подряд; offset/count режут его на срез элемента.
//
// Вершинник по row (из OutPib) читает бит; если стоит — считает rank, берёт index[rank] и
// по нему срез text. Так стоимость масштабируется по тексту, а не по всему миру.
//
// РАЗЛОЖЕНО на 4 буфера, но СТРОИТСЯ одним проходом BuildStaging: bits→wordbase, а offset/count
// получаются из накопления text. Обход — через ForEach (как во всех дата-модулях): он идёт по
// архетипам в map-порядке (= порядок render_instance_base) и по локальному индексу внутри, то
// есть по ВОЗРАСТАНИЮ row, поэтому push_back в index_/text_ автоматически = rank-порядок.
//
// КОНЦЕПТ: UpdateInstruction'ы НЕ заведены. Контракт будущей проводки — как у LightDataModule
// (снапшот в size-фазе): BuildStaging(om) зовётся ОДИН раз в size-фазе ПЕРВОГО UI-буфера
// (напр. bits), после чего остальные Calc*Size/Store* лишь читают уже готовый staging. Пример:
//   bm->CreateUpdateInstruction(UI_TEXT_BITS_BUFFER,
//       [uidm](cp,bm,task){ uidm->StoreBits(bm,&task); },
//       [uidm,om]()->uint32_t { uidm->BuildStaging(om); return uidm->CalcBitsSize(); });
//   // остальные три: updater = Store{WordBase,Index,Text}, size_fn = Calc*Size (без Build).
class UI_DataModule {
public:
	UI_DataModule();

	// Один проход по активной сцене: наполняет все 4 staging-вектора (bits/wordbase/index/text).
	// Пересобирается КАЖДЫЙ кадр (канал зависит от row трансформа, а строки сдвигаются при
	// create/delete/hide любого drawable — гейт по флагу это не ловит). Пропускает не-UI-текст
	// энтити, поэтому цена — по числу текстовых элементов + O(N/32) на битовую часть. Зовётся в
	// size-фазе первого UI-буфера. Оптимизация на будущее — гейт по ревизии батчей (как PIB).
	void BuildStaging(ObjectManager* om);

	// Размеры staging'а в БАЙТАХ (для size-фаз UpdateInstruction). Значения КЭШИРУЮТСЯ в конце
	// BuildStaging — умножение раз на сборку, а не на каждый вызов. Определения в .cpp.
	uint32_t CalcBitsSize()     const;
	uint32_t CalcWordBaseSize() const;
	uint32_t CalcIndexSize()    const;
	uint32_t CalcTextSize()     const;

	// Заливка соответствующего staging-вектора в transfer-буфер таска (одним блобом). Требует
	// уже выполненного BuildStaging этого кадра. Пустой вектор → no-op.
	void StoreBits(BufferManager* bm, UploadTask* task);
	void StoreWordBase(BufferManager* bm, UploadTask* task);
	void StoreIndex(BufferManager* bm, UploadTask* task);
	void StoreText(BufferManager* bm, UploadTask* task);

private:
	// staging: capacity переживает кадры (в steady state без аллокаций — как cams в LightDataModule).
	std::vector<uint32_t> bits_;       // presence, бит/row
	std::vector<uint32_t> wordbase_;   // пословный префикс-popcount
	std::vector<uint32_t> index_;      // пары {offset,count} подряд (2 uint на текст-элемент)
	std::vector<uint32_t> text_;       // коды глифов

	// Кэш размеров staging'а в БАЙТАХ (пишутся в конце BuildStaging; index_ = 2 uint на текст-
	// элемент, поэтому это index_.size()*sizeof(uint32_t), а не число пар). Calc*Size их отдают.
	uint32_t bits_size_     = 0;
	uint32_t wordbase_size_ = 0;
	uint32_t index_size_    = 0;
	uint32_t text_size_     = 0;
};
