#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <SDL3/SDL_pixels.h>

struct DecodedImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::byte> pixels;
	bool ok() const { return width > 0 && height > 0 && !pixels.empty(); }
};

struct DecodedCubeMap {
	uint32_t faceSize = 0;
	std::vector<std::byte> pixels;
	bool ok() const { return faceSize > 0 && !pixels.empty(); }
};

class TextureLoader {
public:
	DecodedImage LoadFromFile(const char* path, SDL_PixelFormat target_format);

	DecodedCubeMap LoadCubeMapFromFile(const char* path, uint32_t faceSize, SDL_PixelFormat target_format);
};
