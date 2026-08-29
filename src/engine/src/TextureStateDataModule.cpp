#include "PCH.h"
#include "TextureStateDataModule.h"
#include "BaseComponents.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "MaterialManager.h"
#include "MaterialData.h"

// Число ячеек, которое сущность занимает в буфере состояний. Секция есть у КАЖДОГО материала,
// включая тот, у которого вариативных ролей нет вовсе: смещение секции шейдер считает как
// material_index * MAX_VARIATIVE_SLOTS, и пропуск сдвинул бы все последующие. Мёртвая секция
// безвредна — у всех её слотов count == 1, и гард в шейдере до неё не доходит.
static inline uint32_t ElementCells(const MaterialComponent& mc)
{
	return safe_u32(mc.materials.size()) * MAX_VARIATIVE_SLOTS;
}

// Слово rank-буфера: биты присутствия + число носителей до этого слова. Пара уходит на GPU как
// StructuredBuffer<uint2>, поэтому раскладка обязана совпадать — отсюда static_assert.
struct RankWord { uint32_t bits; uint32_t base; };
static_assert(sizeof(RankWord) == 2 * sizeof(uint32_t), "rank word must match StructuredBuffer<uint2>");

// Сколько слов копится в локальном буфере до сброса в трансфер. Вызов заливки несёт проверки
// таска и границ буфера-назначения, и платить их поэлементно дорого (ЗАМЕРЕНО на 800k, Release:
// поэлементно 14.0 мс против 1.3 мс пачками). Буфер ОГРАНИЧЕННЫЙ и локальный — это не
// CPU-отражение буфера.
static constexpr size_t RANK_FLUSH_WORDS = 4096;

TextureStateDataModule::TextureStateDataModule()
{
	// ~0 — «в слоте не лежит ничего»: первый же кадр разойдётся с любой настоящей ревизией.
	for (uint64_t& r : last_rank_revision)  r = ~0ull;
	for (uint64_t& r : last_index_revision) r = ~0ull;
}

// Строит канал целиком: домен строк + список носителей. Ни одна строка не опрашивается по
// сущности — вопрос «есть ли тег» задаётся АРХЕТИПУ (фильтром ForEach), а адрес носителя
// считается как «база его архетипа + индекс в архетипе».
//
// Почему не построчный обход с Has<>: такой вопрос задаётся СУЩНОСТИ (поиск в словаре сцены +
// резолв колонки по typeid) и стоит ЗАМЕРЕНО на 800k, Release: 158 нс на строку, 128 мс на
// вызов — при том что голый обход тех же строк это 1.3 мс.
//
// Связь двух обходов — указатель на колонку Positions: ForEachArchetype отдаёт
// ComponentArray<Positions>*, ForEach кладёт &arr->data в SoAElement::soa — это один и тот же
// адрес. Архетипов десятки, поэтому линейный поиск по ним, а не второй индекс.
uint32_t TextureStateDataModule::CalculateRankSize(ObjectManager* om, SceneData* scene, uint64_t revision, uint8_t slot)
{
	if (revision == last_rank_revision[slot]) return 0;
	last_rank_revision[slot] = revision;

	// 1. Базы архетипов в порядке домена строк. Здесь уместен именно ForEachArchetype: нужен
	//    размер КОЛОНКИ, а не обход сущностей.
	struct ArchBase { const Positions* col; uint32_t base; };
	std::vector<ArchBase> bases;
	rows_ = 0;
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<DrawComponent, void>*)
	{
		bases.push_back(ArchBase{ &posArr->data, rows_ });
		rows_ += safe_u32(posArr->size());
	});

	// 2. Носители: тот же обход и в том же порядке, что у StoreState, — поэтому смещение,
	//    посчитанное здесь той же прогрессией, указывает ровно на ячейки, которые там лягут.
	//    Строки выходят по возрастанию: архетипы обходятся в одном порядке, внутри — по индексу.
	hit_rows_.clear();
	hit_ofs_.clear();
	uint32_t running = 0;
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions> pos, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
	{
		const Positions* col = pos.soa;
		for (const ArchBase& a : bases) {
			if (a.col != col) continue;
			hit_rows_.push_back(a.base + safe_u32(pos.i()));
			hit_ofs_.push_back(running);
			break;
		}
		running += ElementCells(mc);
	});

	return RankWords() * safe_u32(sizeof(RankWord));
}

void TextureStateDataModule::StoreRank(BufferManager* bm, UploadTask* task)
{
	// chunk держится нулевым: слово без носителей — это ровно {bits = 0, base = сколько было},
	// поэтому переписывается только base, а биты ставятся лишь у слов с попаданиями.
	const uint32_t words = RankWords();
	std::vector<RankWord> chunk(RANK_FLUSH_WORDS);
	size_t h = 0;
	uint32_t base = 0;

	for (uint32_t w0 = 0; w0 < words; w0 += RANK_FLUSH_WORDS) {
		const uint32_t n = (words - w0 < RANK_FLUSH_WORDS) ? words - w0
		                                                   : safe_u32(RANK_FLUSH_WORDS);
		for (uint32_t i = 0; i < n; ++i) {
			// base — состояние ДО слова, поэтому пишется раньше, чем поглощаются его носители.
			chunk[i].base = base;
			uint32_t bits = 0;
			const uint32_t row_end = (w0 + i + 1u) * 32u;
			while (h < hit_rows_.size() && hit_rows_[h] < row_end) {
				bits |= 1u << (hit_rows_[h] & 31u);
				++h; ++base;
			}
			chunk[i].bits = bits;
		}
		bm->UploadToTransferBuffer(task, safe_u32(size_t(n) * sizeof(RankWord)), chunk.data());
	}
}

// Смещения носителей уже лежат подряд и в нужном порядке — заливаются как есть, одним блобом.
uint32_t TextureStateDataModule::CalculateIndexSize(uint64_t revision, uint8_t slot)
{
	if (revision == last_index_revision[slot]) return 0;
	last_index_revision[slot] = revision;
	return safe_u32(hit_ofs_.size() * sizeof(uint32_t));
}

void TextureStateDataModule::StoreIndex(BufferManager* bm, UploadTask* task)
{
	if (hit_ofs_.empty()) return;
	bm->UploadToTransferBuffer(task, safe_u32(hit_ofs_.size() * sizeof(uint32_t)), hit_ofs_.data());
}

// Домен — носители тега: у остальных элемента нет по определению. Фильтр стоит в списке типов
// ForEach, то есть архетипы без тега отсеиваются целиком, а не построчно, — из-за этого обход и
// дёшев настолько, что гейт ему не нужен (ЗАМЕРЕНО на 800k: 0.001 мс). Резолва материалов не
// требует: длина элемента выводится из КОМПОНЕНТА (сколько у сущности материалов).
uint32_t TextureStateDataModule::CalculateStateSize(ObjectManager* om, SceneData* scene)
{
	uint32_t cells = 0;
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
	{
		cells += ElementCells(mc);
	});

	return cells * sizeof(uint32_t);
}

void TextureStateDataModule::StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om,
	SceneData* scene, MaterialManager* mtm)
{
	// Тот же домен и порядок, что у CalculateStateSize и у обхода носителей в CalculateRankSize:
	// index[rank] указывает ровно на ячейки, которые кладёт этот проход.
	//
	// Абсолютного смещения эта фаза не знает и не должна: секции дописываются ПОДРЯД, курсором
	// служит сам аппенд UploadToTransferBuffer (он двигает task->written_size).
	om->ForEach<Positions, DrawComponent, MaterialComponent, TextureStateComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc, TextureStateComponent&)
	{
		for (const MaterialRef& m : mc.materials) {
			// Нули — это и «слот дефолтный», и хвост секции: массив обнуляется целиком,
			// а заполняются только ячейки вариативных ролей.
			uint32_t cells[MAX_VARIATIVE_SLOTS] = {};

			// Промах имени материала → секция уходит нулями. Через карту, а не GetMaterial:
			// тот логирует промах, а здесь вызов на каждую сущность.
			const Material* mat = nullptr;
			if (mtm && !m.name.empty()) {
				const auto& materials = mtm->GetMaterials();
				auto it = materials.find(m.name);
				if (it != materials.end()) mat = it->second.get();
			}

			if (mat) {
				// Позицию ячейки задаёт ПОРЯДОК (CollectVariativeRoles), а не роль: роль нужна
				// только чтобы достать значение из разреженного states. Никакой семантики модуль
				// не назначает — «ячейка 2 = Emissive» устанавливает пуш.
				const VariativeRoles vr = CollectVariativeRoles(*mat);
				for (uint32_t c = 0; c < vr.count; ++c)
					for (const auto& [role, v] : m.states)
						if (role == vr.role[c]) { cells[c] = v; break; }
			}

			bm->UploadToTransferBuffer(task, sizeof(cells), cells);
		}
	});
}
