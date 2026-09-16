#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <functional>

// Идентификатор ресурса ВНУТРИ одного реестра: это индекс его ячейки. Тег-параметр разводит
// пространства, поэтому id текстуры не подставится туда, где ждут id атласа. 0 — «ссылки нет».
template <class Tag>
struct ResourceId {
	uint32_t v = 0;
	constexpr explicit operator bool() const noexcept { return v != 0; }
	friend constexpr bool operator==(ResourceId a, ResourceId b) noexcept { return a.v == b.v; }
};

template <class Tag>
struct std::hash<ResourceId<Tag>> {
	size_t operator()(ResourceId<Tag> id) const noexcept { return std::hash<uint32_t>{}(id.v); }
};

// Реестр ресурсов: ячейка = {человеческое имя, объект}, а ссылающиеся держат id. Поэтому
// переименование — запись в одно поле ячейки, а не обход всех, кто на ресурс сослался.
//
// Ячейка ПЕРЕЖИВАЕТ удаление объекта, и это условие, а не экономия: id обязан оставаться
// разрешимым в имя (сохранение сцены пишет имена, диагностика их печатает), а пересоздание под
// тем же именем обязано попасть в ТУ ЖЕ ячейку — иначе replace из редактора и снос сценовых
// ресурсов перед загрузкой оставляли бы ссылающихся ни с чем.
//
// deque, а не vector: NameOf отдаёт ссылку, которую UI-поток читает, пока sim может завести
// новый ресурс, — переселение элементов сделало бы её висячей.
template <class Holder, class Tag>
class ResourceRegistry {
public:
	using Id = ResourceId<Tag>;
	using Object = typename std::pointer_traits<Holder>::element_type;

	struct Cell {
		std::string name;
		Holder      object{};
	};

	ResourceRegistry() { cells_.emplace_back(); }

	Id Intern(std::string_view name) {
		if (Id id = Find(name)) return id;
		cells_.push_back(Cell{ std::string(name), Holder{} });
		return Id{ static_cast<uint32_t>(cells_.size() - 1) };
	}
	// Линейно, и этого достаточно: по имени ищут только загрузка манифеста и редактор. Индекс
	// «имя → id» рядом с ячейками был бы вторым источником истины про одно и то же.
	Id Find(std::string_view name) const {
		for (uint32_t i = 1; i < cells_.size(); ++i)
			if (cells_[i].name == name) return Id{ i };
		return Id{};
	}

	const std::string& NameOf(Id id) const {
		static const std::string none;
		return Valid(id) ? cells_[id.v].name : none;
	}
	void Rename(Id id, std::string_view name) { if (Valid(id)) cells_[id.v].name.assign(name); }

	Object* Get(Id id) const {
		return (Valid(id) && cells_[id.v].object) ? &*cells_[id.v].object : nullptr;
	}
	void Put(Id id, Holder obj) { if (Valid(id)) cells_[id.v].object = std::move(obj); }
	bool Erase(Id id) {
		if (!Valid(id) || !cells_[id.v].object) return false;
		cells_[id.v].object = Holder{};
		return true;
	}

	// Индекс ячейки И ЕСТЬ её id, поэтому обход счётчиком: range-for отдал бы ячейку без id.
	uint32_t Count() const { return static_cast<uint32_t>(cells_.size()); }
	const Cell& At(uint32_t i) const { return cells_[i]; }
	Cell& At(uint32_t i) { return cells_[i]; }

private:
	bool Valid(Id id) const { return id.v != 0 && id.v < cells_.size(); }

	std::deque<Cell> cells_;
};
