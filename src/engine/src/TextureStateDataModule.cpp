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
// Гейт «у сущности есть что переключать»: элемента в буфере состояний нет вовсе, пока ни один
// материал сущности ничего не переключил. Ценность — имя и одно определение условия: «пусто»
// здесь значит именно «ни одного непустого states», а не «нет материалов». Считается ПО
// КОМПОНЕНТУ, без резолва материалов, — решение принимается на миллионе строк за копейки.
inline bool HasAnyTextureState(const MaterialComponent& mc) {
	for (const MaterialRef& m : mc.materials) if (!m.states.empty()) return true;
	return false;
}

// Сколько строк префикса копится в локальном буфере до сброса в трансфер. Вызов заливки несёт
// проверки таска и границ буфера-назначения, и на домене в миллион строк платить их построчно
// дорого (ЗАМЕРЕНО на 800k, Release: построчно 14.0 мс против 1.3 мс пачками, при бюджете
// prepare ~11 мс на кадр). Буфер ОГРАНИЧЕННЫЙ и локальный — это не CPU-отражение буфера.
static constexpr size_t PREFIX_FLUSH_ROWS = 4096;

// Размер префикса = размер домена строк, ровно как у InstanceDataModule. Материалы тут ни при
// чём: строка есть у каждой рисуемой сущности, даже если переключать ей нечего (получит -1).
// Здесь уместен именно ForEachArchetype: нужен размер КОЛОНКИ, а не обход сущностей.
uint32_t TextureStateDataModule::CalculatePrefixSize(ObjectManager* om, SceneData* scene)
{
	uint32_t rows = 0;
	om->ForEachArchetype<Positions, DrawComponent>(scene,
		[&](ComponentArray<Positions, void>* posArr,
			ComponentArray<DrawComponent, void>*)
	{
		rows += safe_u32(posArr->size());
	});
	return rows * sizeof(int32_t);
}

// Домен — сущности С материалами: у остальных элемента нет по определению. Резолва материалов
// не требует: длина элемента выводится из КОМПОНЕНТА (сколько у сущности материалов), а есть ли
// элемент вообще — из HasAnyTextureState.
uint32_t TextureStateDataModule::CalculateStateSize(ObjectManager* om, SceneData* scene)
{
	uint32_t cells = 0;
	om->ForEach<Positions, DrawComponent, MaterialComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc)
	{
		if (!HasAnyTextureState(mc)) return;
		cells += ElementCells(mc);
	});

	// Единственный обход, который вынужден трогать MaterialComponent КАЖДОЙ строки, и он дорог:
	// компонент — вектор MaterialRef, то есть 24 байта заголовка на строку ПЛЮС отдельный блок в
	// куче, куда надо шагнуть за states. ЗАМЕРЕНО на 800k (Release): скан одних заголовков 1.8 мс,
	// с разыменованием 5.6 мс — при том что сама заливка префикса (3.2 МБ) стоит 1.6 мс.
	// Поэтому вывод обхода запоминается и переиспользуется в StorePrefix, а не считается дважды.
	any_states = cells != 0;
	return cells * sizeof(uint32_t);
}

void TextureStateDataModule::StorePrefix(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene)
{
	// Домен и ПОРЯДОК обязаны совпадать с CalculatePrefixSize и TransformDataModule: префикс
	// индексируется строкой трансформа, и разъезд нумерации даст чужие варианты без единого
	// симптома в момент ошибки. Поэтому обход идёт по ВСЕМ строкам, а не только по тем, у кого
	// есть материалы: пропуск сущности оставил бы не дырку, а сдвиг всех последующих строк.
	//
	// MaterialComponent в этом домене НЕОБЯЗАТЕЛЕН (рисуемую сущность без него делает, например,
	// форма создания в редакторе), поэтому он спрашивается ПО СУЩНОСТИ, а не стоит в списке типов
	// ForEach: там он выкинул бы такие строки из обхода.
	std::vector<int32_t> chunk;
	chunk.reserve(PREFIX_FLUSH_ROWS);
	uint32_t running = 0;

	auto flush = [&] {
		if (chunk.empty()) return;
		bm->UploadToTransferBuffer(task, safe_u32(chunk.size() * sizeof(int32_t)), chunk.data());
		chunk.clear();
	};

	om->ForEach<Positions, DrawComponent>(scene,
		[&](Entity e, SoAElement<Positions>, DrawComponent&)
	{
		int32_t ofs = -1;
		// Быстрый путь: во всей сцене никто ничего не переключал → весь буфер это -1, и ни
		// спрашивать компонент, ни шагать за ним в кучу незачем. Это НЕ эвристика: any_states
		// посчитан точно, тем самым обходом, что дал размер второго буфера. Типовой случай —
		// именно он: варианты у материала есть, а переключил их кто-то один или никто.
		// Полагаться на то, что size уже отработал, можно: движок гоняет ВСЕ size_fn и только
		// потом все updater'ы (_ExecuteUpdateInstructions) — от порядка РЕГИСТРАЦИИ это не зависит.
		if (any_states && om->Has<MaterialComponent>(scene, e)) {
			const MaterialComponent& mc = om->GetComponent<MaterialComponent>(scene, e);
			if (HasAnyTextureState(mc)) {
				ofs = safe_u32t_i(running);
				running += ElementCells(mc);
			}
		}
		chunk.push_back(ofs);
		if (chunk.size() >= PREFIX_FLUSH_ROWS) flush();
	});
	flush();
}

void TextureStateDataModule::StoreState(BufferManager* bm, UploadTask* task, ObjectManager* om,
	SceneData* scene, MaterialManager* mtm)
{
	// Тот же домен и порядок, что у CalculateStateSize; со StorePrefix он согласован тем, что
	// подпоследовательность сущностей С материалами в обоих обходах одна и та же (архетипы в
	// одном порядке, внутри архетипа — по индексу).
	//
	// Абсолютного смещения эта фаза не знает и не должна: секции дописываются ПОДРЯД, курсором
	// служит сам аппенд UploadToTransferBuffer (он двигает task->written_size).
	om->ForEach<Positions, DrawComponent, MaterialComponent>(scene,
		[&](SoAElement<Positions>, DrawComponent&, MaterialComponent& mc)
	{
		if (!HasAnyTextureState(mc)) return;

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
