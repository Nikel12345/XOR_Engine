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
	const uint32_t row = out.width * SDL_BYTESPERPIXEL(target_format);
	out.pixels.resize((size_t)row * out.height);

	const std::byte* src = static_cast<const std::byte*>(converted->pixels);
	std::byte* dst = out.pixels.data();
	for (uint32_t y = 0; y < out.height; ++y) {
		SDL_memcpy(dst + (size_t)y * row, src + (size_t)y * converted->pitch, row);
	}

	SDL_DestroySurface(converted);
	return out;
}

DecodedCubeMap TextureLoader::LoadCubeMapFromFile(const char* path, uint32_t faceSize, SDL_PixelFormat target_format)
{
	DecodedCubeMap out;
	if (faceSize == 0) { SDL_Log("LoadCubeMapFromFile: zero faceSize for '%s'", path); return out; }

	DecodedImage img = LoadFromFile(path, target_format);
	if (!img.ok()) { SDL_Log("LoadCubeMapFromFile: failed to load '%s'", path); return out; }

	const uint32_t cellW = img.width / 4;
	const uint32_t cellH = img.height / 3;
	if (cellW == 0 || cellH == 0) { SDL_Log("LoadCubeMapFromFile: '%s' too small for 4x3 cross (%ux%u)", path, img.width, img.height); return out; }

	const uint32_t bpp = SDL_BYTESPERPIXEL(target_format);
	const uint32_t srcStride = img.width * bpp;
	// Ячейка креста (col,row) для слоя SDL cube: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z.
	// Полоса -X +Z +X -Z даёт непрерывный горизонт; +Y верх (col1,row0), -Y низ (col1,row2).
	const int cell[6][2] = { {2,1}, {0,1}, {1,0}, {1,2}, {1,1}, {3,1} };
	const size_t face_bytes = (size_t)faceSize * faceSize * bpp;
	out.pixels.resize(face_bytes * 6);   // грани стопкой: смещение грани f = f·face_bytes
	for (int f = 0; f < 6; ++f) {
		const uint32_t ox = (uint32_t)cell[f][0] * cellW;
		const uint32_t oy = (uint32_t)cell[f][1] * cellH;
		std::byte* face = out.pixels.data() + (size_t)f * face_bytes;
		for (uint32_t dj = 0; dj < faceSize; ++dj) {
			uint32_t sy = oy + (uint32_t)(((dj + 0.5f) / faceSize) * cellH);
			if (sy >= img.height) sy = img.height - 1;
			for (uint32_t di = 0; di < faceSize; ++di) {
				uint32_t sx = ox + (uint32_t)(((di + 0.5f) / faceSize) * cellW);
				if (sx >= img.width) sx = img.width - 1;
				const std::byte* s = img.pixels.data() + (size_t)sy * srcStride + (size_t)sx * bpp;
				std::byte* d = face + ((size_t)dj * faceSize + di) * bpp;
				SDL_memcpy(d, s, bpp);   // пиксель как есть (формат совпал с атласом)
			}
		}
	}
	out.faceSize = faceSize;
	return out;
}
