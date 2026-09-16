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

// Грани стопкой в одном буфере: грань f по смещению f·faceSize²·bpp, порядок — слои SDL cube
// (0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z). Это ровно та раскладка, которую ждёт многослойная заливка.
struct DecodedCubeMap {
	uint32_t faceSize = 0;
	std::vector<std::byte> pixels;
	bool ok() const { return faceSize > 0 && !pixels.empty(); }
};

// Единственное место в движке, зависящее от SDL3_image.
class TextureLoader {
public:
	DecodedImage LoadFromFile(const char* path, SDL_PixelFormat target_format);

	// Горизонтальный крест 4×3, ячейка которого ресэмплится в грань faceSize×faceSize:
	//   .  +Y  .   .
	//  -X  +Z +X  -Z
	//   .  -Y  .   .
	DecodedCubeMap LoadCubeMapFromFile(const char* path, uint32_t faceSize, SDL_PixelFormat target_format);
};
