#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <SDL3/SDL_pixels.h>

// Результат декодирования картинки: BGRA32, плотно упакованный (width*height*4).
struct DecodedImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::byte> pixels;
	bool ok() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// Единственное место в движке, зависящее от SDL3_image. Декодит файл с диска
// ОДИН раз в target_format; пиксели уходят в TextureManager без повторного чтения.
// target_format задаёт вызывающий, выводя его из формата целевого атласа.
class TextureLoader {
public:
	DecodedImage LoadFromFile(const char* path, SDL_PixelFormat target_format);
};
