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

struct TextureIdTag;
struct AtlasIdTag;

// -1, а не 0: нулевая ячейка реестра — обычный ресурс, заглушки под «ссылки нет» в нём нет.
template <class Tag>
struct ResourceId {
	int32_t v = -1;
	explicit operator bool() const { return v >= 0; }
	bool operator==(const ResourceId&) const = default;
};

template <class Tag> struct std::hash<ResourceId<Tag>> {
	size_t operator()(ResourceId<Tag> id) const noexcept { return std::hash<int32_t>{}(id.v); }
};

using TextureId = ResourceId<TextureIdTag>;
using AtlasId   = ResourceId<AtlasIdTag>;

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