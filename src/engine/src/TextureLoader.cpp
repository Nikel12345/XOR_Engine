#include "PCH.h"
#include "TextureLoader.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

DecodedImage TextureLoader::LoadFromFile(const char* path, SDL_PixelFormat target_format)
{
	DecodedImage out;

	SDL_Surface* surface = IMG_Load(path);
	if (!surface) {
		SDL_Log("TextureLoader: failed to load image '%s': %s", path, SDL_GetError());
		return out;
	}

	SDL_Surface* converted = SDL_ConvertSurface(surface, target_format);
	SDL_DestroySurface(surface);
	if (!converted) {
		SDL_Log("TextureLoader: conversion failed for '%s': %s", path, SDL_GetError());
		return out;
	}

	out.width = (uint32_t)converted->w;
	out.height = (uint32_t)converted->h;
	const uint32_t row_bytes = out.width * SDL_BYTESPERPIXEL(target_format);
	out.pixels.resize((size_t)row_bytes * out.height);

	const std::byte* src = static_cast<const std::byte*>(converted->pixels);
	std::byte* dst = out.pixels.data();
	for (uint32_t y = 0; y < out.height; ++y)
		SDL_memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * converted->pitch, row_bytes);

	SDL_DestroySurface(converted);
	return out;
}

DecodedCubeMap TextureLoader::LoadCubeMapFromFile(const char* path, uint32_t faceSize, SDL_PixelFormat target_format)
{
	DecodedCubeMap out;
	if (faceSize == 0) { SDL_Log("LoadCubeMapFromFile: zero faceSize for '%s'", path); return out; }

	DecodedImage img = LoadFromFile(path, target_format);
	if (!img.ok()) { SDL_Log("LoadCubeMapFromFile: failed to load '%s'", path); return out; }

	const uint32_t cell_width  = img.width / 4;
	const uint32_t cell_height = img.height / 3;
	if (cell_width == 0 || cell_height == 0) { SDL_Log("LoadCubeMapFromFile: '%s' too small for 4x3 cross (%ux%u)", path, img.width, img.height); return out; }

	const uint32_t bpp = SDL_BYTESPERPIXEL(target_format);
	const uint32_t src_stride = img.width * bpp;
	const int cross_cell[6][2] = { {2,1}, {0,1}, {1,0}, {1,2}, {1,1}, {3,1} };
	const size_t face_bytes = (size_t)faceSize * faceSize * bpp;
	out.pixels.resize(face_bytes * 6);
	for (int face_index = 0; face_index < 6; ++face_index) {
		const uint32_t cell_x = (uint32_t)cross_cell[face_index][0] * cell_width;
		const uint32_t cell_y = (uint32_t)cross_cell[face_index][1] * cell_height;
		std::byte* face = out.pixels.data() + (size_t)face_index * face_bytes;
		for (uint32_t dst_y = 0; dst_y < faceSize; ++dst_y) {
			uint32_t src_y = cell_y + (uint32_t)(((dst_y + 0.5f) / faceSize) * cell_height);
			if (src_y >= img.height) src_y = img.height - 1;
			for (uint32_t dst_x = 0; dst_x < faceSize; ++dst_x) {
				uint32_t src_x = cell_x + (uint32_t)(((dst_x + 0.5f) / faceSize) * cell_width);
				if (src_x >= img.width) src_x = img.width - 1;
				const std::byte* src_pixel = img.pixels.data() + (size_t)src_y * src_stride + (size_t)src_x * bpp;
				std::byte* dst_pixel = face + ((size_t)dst_y * faceSize + dst_x) * bpp;
				SDL_memcpy(dst_pixel, src_pixel, bpp);
			}
		}
	}
	out.faceSize = faceSize;
	return out;
}
