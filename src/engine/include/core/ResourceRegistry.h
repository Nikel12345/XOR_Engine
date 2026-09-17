#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include "Utils.h"
#include "ResourceId.h"

enum class NameSlot { Keep, Release };

template <class Cell, class Id>
class ResourceRegistry {
public:
	Id Intern(std::string_view name) {
		if (name.empty()) return Id{};
		if (Id id = Slot(name)) return id;
		cells.push_back(Cell{ std::string(name), {} });
		return Id{ Count() - 1 };
	}
	Id Find(std::string_view name) const {
		for (int32_t i = 0; i < Count(); ++i)
			if (cells[i].object && cells[i].name == name) return Id{ i };
		return Id{};
	}

	const std::string& NameOf(Id id) const {
		static const std::string none;
		return Valid(id) ? cells[Index(id)].name : none;
	}
	bool Rename(Id id, std::string_view name) {
		if (!Valid(id) || name.empty()) return false;
		for (int32_t i = 0; i < Count(); ++i) {
			if (Id{ i } == id || cells[i].name != name) continue;
			if (cells[i].object) return false;
			cells[i].name.clear();
		}
		cells[Index(id)].name.assign(name);
		return true;
	}

	auto* Get(Id id) const { return Valid(id) ? cells[Index(id)].object.get() : nullptr; }
	void Put(Id id, decltype(Cell::object) object) { if (Valid(id)) cells[Index(id)].object = std::move(object); }

	bool Clear(Id id) {
		if (!Valid(id) || !cells[Index(id)].object) return false;
		cells[Index(id)].object.reset();
		return true;
	}
	bool Drop(Id id) {
		if (!Clear(id)) return false;
		cells[Index(id)].name.clear();
		return true;
	}

	int32_t Count() const { return safe_size_i(cells.size()); }
	const Cell& At(int32_t i) const { return cells[i]; }

private:
	Id Slot(std::string_view name) const {
		for (int32_t i = 0; i < Count(); ++i)
			if (!cells[i].name.empty() && cells[i].name == name) return Id{ i };
		return Id{};
	}
	bool Valid(Id id) const { return id.v >= 0 && id.v < Count(); }
	size_t Index(Id id) const { return safe_i_u32(id.v); }

	std::deque<Cell> cells;
};
