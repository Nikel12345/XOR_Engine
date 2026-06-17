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
