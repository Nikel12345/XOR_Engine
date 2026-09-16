#pragma once
#include <string>
#include <cstdint>
#include <functional>

using RenderPassName = std::string;
using ComputePassName = std::string;
using ComputePrepassName = std::string;
using BlitPassName = std::string;
using SceneName = std::string;
using MaterialName = std::string;
using ModelName = std::string;
using AtlasName = std::string;
using TextureName = std::string;
using ShaderName = std::string;

using BufferDataName = const char*;

// Ссылка на ресурс = индекс его ЯЧЕЙКИ в реестре менеджера; -1 — ссылки нет. Тип на каждый
// реестр свой, поэтому id текстуры не подставится туда, где ждут id атласа.
struct TextureId {
	int32_t v = -1;
	explicit operator bool() const { return v >= 0; }
	bool operator==(const TextureId&) const = default;
};
struct AtlasId {
	int32_t v = -1;
	explicit operator bool() const { return v >= 0; }
	bool operator==(const AtlasId&) const = default;
};

template <> struct std::hash<TextureId> {
	size_t operator()(TextureId id) const noexcept { return std::hash<int32_t>{}(id.v); }
};

namespace BatchKeys {
	using ModelBatchKey = uint64_t;
	using TextureBatchKey = uint64_t;
	using AtlasBatchKey = uint64_t;
	using ShaderBatchKey = uint64_t;
	// Пара (материал, sp) — единица ПЕР-МАТЕРИАЛЬНОЙ половины батча (MatSpLayout): всё, что
	// texture-батч берёт из материала и не зависит от сущности. Ключ памятки предпрохода И
	// ресурсный вклад в TextureBatchKey (см. BatchBuilder: HashMatSpMemo/HashMatSpResources).
	using MatSpKey = uint64_t;
};