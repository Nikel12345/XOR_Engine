#pragma once
#include <cstdint>
#include <functional>

struct TextureIdTag;
struct AtlasIdTag;
struct VertexShaderIdTag;
struct FragmentShaderIdTag;
struct ComputeShaderIdTag;

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

using TextureId        = ResourceId<TextureIdTag>;
using AtlasId          = ResourceId<AtlasIdTag>;
using VertexShaderId   = ResourceId<VertexShaderIdTag>;
using FragmentShaderId = ResourceId<FragmentShaderIdTag>;
using ComputeShaderId  = ResourceId<ComputeShaderIdTag>;
