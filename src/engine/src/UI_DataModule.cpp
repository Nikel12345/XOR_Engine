#include "PCH.h"
#include "UI_DataModule.h"
#include "Utils.h"           // safe_u32
#include "BufferManager.h"   // UploadToTransferBuffer, UploadTask
#include "ObjectManager.h"   // ForEach/ForEachArchetype/Has, GetActiveScene, SceneData
#include <bit>               // std::popcount

UI_DataModule::UI_DataModule() {}

void UI_DataModule::BuildStaging(ObjectManager* om)
{
	SceneData* scene = om ? om->GetActiveScene() : nullptr;
	if (!scene) return;

	// Пересобираем КАЖДЫЙ кадр: канал индексируется row'ом трансформа, а строки сдвигаются при
	// create/delete/hide ЛЮБОГО drawable (не только UI). Гейт по одному флагу это не ловит — стейл
	// row → текст «пропадает». Дёшево: не-UI архетипы пропускаются, битовая часть O(N/32).
	bits_.clear();
	wordbase_.clear();
	index_.clear();
	text_.clear();
	bits_size_ = wordbase_size_ = index_size_ = text_size_ = 0;   // на случай ранних выходов ниже

	// 1) N = число строк ТОЛЬКО архетипов Positions+DrawComponent — это ровно ROW-пространство.
	//    Безпозиционные рисуемые (скайбокс, фрактал-квад) СЮДА НЕ ВХОДЯТ намеренно: у них нет
	//    трансформ-строки (RecalculateInstanceOffsets им base не даёт, PIB пишет им -1, вершинник
	//    гардит row<0). Считать их = раздуть N и разъехаться с row. Тот же фильтр, что у
	//    TransformDataModule/InstanceDataModule/BoundSphereDataModule. Битовая маска покрывает row.
	uint32_t N = 0;
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>* posArr, ComponentArray<DrawComponent, void>*)
		{
			N += safe_u32(posArr->size());
		});
	if (N == 0) return;

	const uint32_t words = (N + 31u) / 32u;
	bits_.assign(words, 0u);
	wordbase_.assign(words, 0u);

	// 2) UI-текст в ROW-порядке. ForEach идёт по архетипам в map-порядке (= порядок
	//    render_instance_base) и по локальному индексу внутри — значит по возрастанию row,
	//    поэтому push_back в index_/text_ = rank-порядок (без сортировки). UIComponent/
	//    UITextComponent — обычные AoS-компоненты, приходят прямыми ссылками, без SoAElement.
	//    Positions требуем через Has: безпозиционный UI-элемент (напр. фуллскрин-фон, строящий
	//    NDC-квад сам) трансформ-строки не имеет (его PIB = -1) → в bit-массив он не попадает,
	//    пропускаем. Симметрично тому, как N выше исключает безпозиционных из row-пространства.
	om->ForEach<DrawComponent, UIComponent, UITextComponent>(scene,
		[&](Entity e, DrawComponent&, UIComponent&, UITextComponent& txt)
		{
			if (!om->Has<Positions>(scene, e)) return;
			const uint32_t row = scene->entity_to_archetype[e]->render_instance_base
			                   + safe_u32(scene->entity_to_index[e]);
			bits_[row >> 5] |= (1u << (row & 31u));

			const uint32_t offset = safe_u32(text_.size());
			const uint32_t count  = safe_u32(txt.glyphs.size());
			text_.insert(text_.end(), txt.glyphs.begin(), txt.glyphs.end());
			index_.push_back(offset);   // пара {offset,count} подряд (индекс пары = rank)
			index_.push_back(count);
		});

	// 3) wordbase = пословный префикс-popcount: rank(row) = wordbase[w] + popcount(биты ниже b).
	uint32_t acc = 0;
	for (uint32_t w = 0; w < words; ++w) {
		wordbase_[w] = acc;
		acc += static_cast<uint32_t>(std::popcount(bits_[w]));
	}

	// 4) Кэш байтовых размеров — умножение раз на сборку, Calc*Size их просто отдают.
	bits_size_     = safe_u32(bits_.size()     * sizeof(uint32_t));
	wordbase_size_ = safe_u32(wordbase_.size() * sizeof(uint32_t));
	index_size_    = safe_u32(index_.size()    * sizeof(uint32_t));
	text_size_     = safe_u32(text_.size()     * sizeof(uint32_t));
}

uint32_t UI_DataModule::CalcBitsSize()     const { return bits_size_; }
uint32_t UI_DataModule::CalcWordBaseSize() const { return wordbase_size_; }
uint32_t UI_DataModule::CalcIndexSize()    const { return index_size_; }
uint32_t UI_DataModule::CalcTextSize()     const { return text_size_; }

void UI_DataModule::StoreBits(BufferManager* bm, UploadTask* task)
{
	if (bits_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcBitsSize(), bits_.data());
}

void UI_DataModule::StoreWordBase(BufferManager* bm, UploadTask* task)
{
	if (wordbase_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcWordBaseSize(), wordbase_.data());
}

void UI_DataModule::StoreIndex(BufferManager* bm, UploadTask* task)
{
	if (index_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcIndexSize(), index_.data());
}

void UI_DataModule::StoreText(BufferManager* bm, UploadTask* task)
{
	if (text_.empty()) return;
	bm->UploadToTransferBuffer(task, CalcTextSize(), text_.data());
}
