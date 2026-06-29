#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <SDL3/SDL_pixels.h>

// Результат декодирования картинки: BGRA32, плотно упакованный (width*height*4).
struct DecodedImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::byte> pixels;
	bool ok() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// 6 граней кубмапа: каждая faceSize×faceSize в target_format, плотно упакована (faceSize²·bpp).
// Порядок — слои SDL cube: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z. Это ЧИСТЫЕ пиксели; в GPU-текстуры
// их превращает вызывающий через TextureManager::CreateTexture.
struct DecodedCubeFaces {
	uint32_t faceSize = 0;
	std::array<std::vector<std::byte>, 6> faces;
	bool ok() const { return faceSize > 0 && !faces[0].empty(); }
};

// Единственное место в движке, зависящее от SDL3_image. Декодит файл с диска
// ОДИН раз в target_format; пиксели уходят в TextureManager без повторного чтения.
// target_format задаёт вызывающий, выводя его из формата целевого атласа.
class TextureLoader {
public:
	DecodedImage LoadFromFile(const char* path, SDL_PixelFormat target_format);

	// Декодит "плоский" скайбокс — горизонтальный крест 4×3:
	//   .  +Y  .   .
	//  -X  +Z +X  -Z
	//   .  -Y  .   .
	// — и режет на 6 граней SDL-cube, ресэмпля каждую ячейку креста в квадрат faceSize×faceSize.
	// ВОЗВРАЩАЕТ голые пиксели (по набору на грань) — GPU-ресурсов не трогает: создание текстур
	// остаётся за TextureManager. faceSize и target_format задаёт вызывающий, выводя их из tci/формата
	// целевого cube-атласа (единственный источник истины о разрешении — без магических чисел здесь).
	DecodedCubeFaces LoadCubeMapFromFile(const char* path, uint32_t faceSize, SDL_PixelFormat target_format);
};
