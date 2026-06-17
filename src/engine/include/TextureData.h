#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>

struct TextureData {
	uint32_t uv_packed_offset;  // unorm16 × 2: offset_x в low, offset_y в high
	uint32_t uv_packed_scale;   // unorm16 × 2: scale_x в low, scale_y в high
	uint32_t layer;
	uint32_t _pad;
};

struct TextureAtlas{
	std::vector<std::unique_ptr<TextureData>> textures_data;
	SDL_GPUTextureSamplerBinding texture_binding;
	SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
	uint32_t width = 0;
	uint32_t height = 0;
	uint16_t layers = 0;
	uint8_t padding = 3; // Пространство между текстурами в атласе
	uint8_t mip_levels = 1;
};

//struct TextureData {
//	SDL_GPUTextureSamplerBinding texture;
//	uint32_t w = 0;
//	uint32_t h = 0;
//};


struct TextureHandle {
	TextureAtlas* atlas = nullptr;
	TextureData* texture_data = nullptr;
	uint32_t width = 0;   // нативный размер картинки в пикселях, пишется при загрузке
	uint32_t height = 0;  // (точный исходник, без round-trip через unorm16 uv_packed_scale)
};