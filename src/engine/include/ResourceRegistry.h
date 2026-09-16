#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include "Utils.h"

// Реестр ресурсов менеджера: ячейка = {человеческое имя, объект}, а ссылающиеся держат её id.
// Поэтому переименование — запись в одно поле ячейки, а не обход всех, кто на ресурс сослался.
//
// Ячейка ПЕРЕЖИВАЕТ удаление объекта, и это условие, а не экономия: id обязан оставаться
// разрешимым в имя (сохранение сцены пишет имена, диагностика их печатает), а пересоздание под
// тем же именем обязано попасть в ТУ ЖЕ ячейку — иначе replace из редактора и снос сценовых
// ресурсов перед загрузкой оставляли бы ссылающихся ни с чем. Отсюда же второе: ячейки не
// удаляются и не переставляются, поэтому id никогда не достаётся другому ресурсу — протухшая
// ссылка либо пуста, либо ведёт туда же, куда вела. Подменять нечем, счётчик поколений не нужен.
//
// deque, а не vector: NameOf отдаёт ссылку, которую UI-поток читает, пока sim может завести
// новый ресурс, — переселение элементов сделало бы её висячей.
//
// Cell параметром, а не объявлением внутри: у ячеек разных реестров разное владение
// (shared_ptr у хэндла, unique_ptr у атласа), и вызывающему нужно её имя без вложенности.
template <class Cell, class Id>
class ResourceRegistry {
public:
	// Ячейка имени; заводит её, если имени ещё не было. Объекта в ячейке может не быть — так
	// ссылка на ещё не созданный ресурс остаётся законной, и порядок загрузки манифестов не значим.
	Id Intern(std::string_view name) {
		if (Id id = Find(name)) return id;
		cells.push_back(Cell{ std::string(name), {} });
		return Id{ Count() - 1 };
	}
	// Линейно, и этого достаточно: по имени ищут только загрузка манифеста и редактор. Индекс
	// «имя → id» рядом с ячейками был бы вторым источником истины про одно и то же.
	Id Find(std::string_view name) const {
		for (int32_t i = 0; i < Count(); ++i)
			if (cells[i].name == name) return Id{ i };
		return Id{};
	}

	const std::string& NameOf(Id id) const {
		static const std::string none;
		return Valid(id) ? cells[Index(id)].name : none;
	}
	void Rename(Id id, std::string_view name) { if (Valid(id)) cells[Index(id)].name.assign(name); }

	auto* Get(Id id) const { return Valid(id) ? cells[Index(id)].object.get() : nullptr; }
	// Кладёт объект в УЖЕ заведённую ячейку: id и имя выдал Intern, объект строится после него.
	void Put(Id id, decltype(Cell::object) object) { if (Valid(id)) cells[Index(id)].object = std::move(object); }
	// Объект уходит, имя остаётся (см. комментарий к классу).
	bool Erase(Id id) {
		if (!Valid(id) || !cells[Index(id)].object) return false;
		cells[Index(id)].object.reset();
		return true;
	}

	// Обход: Count — число ЯЧЕЕК, а не живых ресурсов (опустевшие считаются тоже), а индекс
	// ячейки и есть значение её id. Пустые ячейки отсеивает вызывающий, проверяя .object.
	int32_t Count() const { return safe_size_i(cells.size()); }
	const Cell& At(int32_t i) const { return cells[i]; }

private:
	bool Valid(Id id) const { return id.v >= 0 && id.v < Count(); }
	size_t Index(Id id) const { return safe_i_u32(id.v); }

	std::deque<Cell> cells;
};
