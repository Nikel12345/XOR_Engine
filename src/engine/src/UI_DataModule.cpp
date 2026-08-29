#include "PCH.h"
#include "BaseComponents.h"
#include "UI_DataModule.h"
#include "Utils.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include "SparseRankChannel.h"

UI_DataModule::UI_DataModule() {}

void UI_DataModule::BuildStaging(ObjectManager* om)
{
	SceneData* scene = om ? om->GetActiveScene() : nullptr;
	if (!scene) return;

	// Пересобираем КАЖДЫЙ кадр: канал индексируется row'ом трансформа, а строки сдвигаются при
	// create/delete/hide ЛЮБОГО drawable (не только UI). Гейт по одному флагу это не ловит — стейл
	// row → текст «пропадает». Дёшево: не-UI архетипы пропускаются, битовая часть O(N/32).
	hit_rows_.clear();
	index_.clear();
	text_.clear();
	rows_ = 0;
	rank_size_ = index_size_ = text_size_ = 0;   // на случай ранних выходов ниже

	// 1) N = число строк ТОЛЬКО архетипов Positions+DrawComponent — это ровно ROW-пространство.
	//    Безпозиционные рисуемые (скайбокс, фрактал-квад) СЮДА НЕ ВХОДЯТ намеренно: у них нет
	//    трансформ-строки (RecalculateInstanceOffsets им base не даёт, PIB пишет им -1, вершинник
	//    гардит row<0). Считать их = раздуть N и разъехаться с row. Тот же фильтр, что у
	//    TransformDataModule/InstanceDataModule/BoundSphereDataModule. Битовая маска покрывает row.
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>* posArr, ComponentArray<DrawComponent, void>*)
		{
			rows_ += safe_u32(posArr->size());
		});
	if (rows_ == 0) return;

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
			hit_rows_.push_back(scene->entity_to_archetype[e]->render_instance_base
			                    + safe_u32(scene->entity_to_index[e]));

			const uint32_t offset = safe_u32(text_.size());
			const uint32_t count  = safe_u32(txt.glyphs.size());
			text_.insert(text_.end(), txt.glyphs.begin(), txt.glyphs.end());
			index_.push_back(offset);   // пара {offset,count} подряд (индекс пары = rank)
			index_.push_back(count);
		});

	// 3) Кэш байтовых размеров — умножение раз на сборку, Calc*Size их просто отдают. Слова канала
	//    не собираются здесь вовсе: их раскладывает StoreSparseRank прямо в трансфер.
	rank_size_  = SparseRankBytes(rows_);
	index_size_ = safe_u32(index_.size() * sizeof(uint32_t));
	text_size_  = safe_u32(text_.size()  * sizeof(uint32_t));
}

uint32_t UI_DataModule::CalcRankSize()  const { return rank_size_; }
uint32_t UI_DataModule::CalcIndexSize() const { return index_size_; }
uint32_t UI_DataModule::CalcTextSize()  const { return text_size_; }

void UI_DataModule::StoreRank(BufferManager* bm, UploadTask* task)
{
	StoreSparseRank(bm, task, rows_, hit_rows_);
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
