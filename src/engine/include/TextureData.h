#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>
#include <string>
#include "ResourceTags.h"
#include "ResourceId.h"

enum class ChannelConvention {
	AsIs,
	SmoothnessInGreen,   // G = smoothness → инвертируется в roughness
	DepthInAlpha,        // A = depth/cavity → инвертируется в height
};

struct TextureData {
	uint16_t uv_offset_x = 0;   // unorm16 от стороны атласа
	uint16_t uv_offset_y = 0;
	uint16_t uv_scale_x = 0;
	uint16_t uv_scale_y = 0;
	uint32_t layer = 0;
	uint32_t layer_span = 1;
};

struct TextureAtlas{
	ResourceTag tags = ResourceTag::None;

	std::vector<TextureData*> textures;
	SDL_GPUTextureSamplerBinding texture_binding;
	SDL_GPUTextureCreateInfo tci{};
	std::string name;
	TextureAtlas* shares_with = nullptr;
	SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureType texture_type = SDL_GPU_TEXTURETYPE_2D;
	uint32_t width = 0;
	uint32_t height = 0;
	uint16_t layers = 0;
	uint8_t padding = 0;
	uint8_t mip_levels = 1;
};

struct TextureHandle {
	TextureAtlas* atlas = nullptr;
	TextureData texture_data{};
	uint32_t width = 0;
	uint32_t height = 0;

	AtlasId           atlas_id;
	// Пуст у текстур из сырых пикселей: из файла они не пересоздаются, поэтому и в манифест
	// сцены не идут, и снос сцены их не трогает.
	std::string       source_path;
	ChannelConvention conv = ChannelConvention::AsIs;
	ResourceTag       tags = ResourceTag::None;
};

class TextureManager;