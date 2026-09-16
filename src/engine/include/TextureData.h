#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>
#include <string>
#include "ResourceTags.h"
#include "ResourceId.h"

// Конвенция упаковки ИСХОДНОГО файла: канон движка — G несёт linear roughness, альфа нормал-карты
// несёт height. Инверсия формат-независима: G = индекс 1, A = 3 и в RGBA, и в BGRA.
enum class ChannelConvention {
	AsIs,
	SmoothnessInGreen,   // G = smoothness → инвертируется в roughness
	DepthInAlpha,        // A = depth/cavity → инвертируется в height
};

struct TextureData {
	uint32_t uv_packed_offset = 0;  // unorm16 × 2: offset_x в low, offset_y в high
	uint32_t uv_packed_scale  = 0;  // unorm16 × 2: scale_x в low, scale_y в high
	uint32_t layer            = 0;
	uint32_t layer_span       = 1;   // подряд идущих слоёв от layer: 1 у текстуры, 6 у кубмапы
};


struct TextureAtlas{
	ResourceTag tags = ResourceTag::None;

	std::vector<TextureData*> textures;   // НЕвладеющий; владелец записей — сами хэндлы
	// .texture пуст до бейка, поэтому копировать биндинг на setup нельзя — держи TextureAtlas*
	// и резолви на исполнении.
	SDL_GPUTextureSamplerBinding texture_binding;
	// Источник истины об usage и размерах: по нему создаёт бейк и пересоздаёт ресайз.
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

struct TextureHandle : std::enable_shared_from_this<TextureHandle> {
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