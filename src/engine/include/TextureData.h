#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>

// Конвенция упаковки ИСХОДНОГО файла текстуры. Канон движка: G = linear roughness (ORM),
// A нормал-карты = HEIGHT (яркое = выше; POM марчит depth = 1 - A).
// Импорт нормализует к канону один раз на CPU (см. EngineContext::CreateTextureFromFile),
// поэтому шейдер/материалы всегда видят единую конвенцию — без рантайм-веток и флагов на материале.
// Индексы каналов одинаковы в RGBA и BGRA (G = 1, A = 3), так что инверсии формат-независимы.
enum class ChannelConvention {
	AsIs,                // без изменений (по умолчанию) — файл уже в каноне движка
	SmoothnessInGreen,   // G = smoothness/glossiness (Unity-стиль) → инвертируется в roughness (G = 255 - G)
	DepthInAlpha,        // A = depth/cavity (яркое = ГЛУБЖЕ, напр. wood_normal_h) → инвертируется в height (A = 255 - A)
};

struct TextureData {
	uint32_t uv_packed_offset;  // unorm16 × 2: offset_x в low, offset_y в high
	uint32_t uv_packed_scale;   // unorm16 × 2: scale_x в low, scale_y в high
	uint32_t layer;
	uint32_t _pad;
};

struct TextureAtlas{
	// НЕвладеющий список размещений текстур в атласе. Держим именно TextureData (а не TextureHandle):
	// атлас — тонкая прослойка над GPU-текстурой, ему нужны только регионы (UVL+layer). Владелец
	// самих TextureData — TextureManager::handles_data (по значению внутри TextureHandle, адрес
	// стабилен). Пусто для атласов-таргетов, где хэндлы не создавались. Регион каждого элемента
	// точно распаковывается из UVL (unorm16, пиксель-в-пиксель) — этого достаточно для пересборки
	// свободного места при удалении и для гипотетической GPU→GPU перепаковки.
	std::vector<TextureData*> textures;
	SDL_GPUTextureSamplerBinding texture_binding;
	SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureType texture_type = SDL_GPU_TEXTURETYPE_2D; // 2D / CUBE / ARRAY — берётся из tci при создании; компатибилити-проверки (это куб?) смотрят сюда
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


// enable_shared_from_this: владелец — TextureManager::handles_data (shared_ptr). Материалы держат
// weak_ptr на хэндл (см. Material::textures) и потому умеют детектировать его удаление. weak_from_this()
// позволяет выдать такой weak_ptr из сырого TextureHandle* без протаскивания shared_ptr через API.
struct TextureHandle : std::enable_shared_from_this<TextureHandle> {
	TextureAtlas* atlas = nullptr;
	TextureData texture_data{};   // GPU-раскладка UVL, ПО ЗНАЧЕНИЮ: handle владеет своей записью,
	                              // адрес стабилен (handles_data держит shared_ptr<TextureHandle>).
	uint32_t width = 0;   // нативный размер картинки в пикселях, пишется при загрузке
	uint32_t height = 0;  // (точный исходник, без round-trip через unorm16 uv_packed_scale)
};

class TextureManager;

// Depth-таргет, разделяемый несколькими проходами и привязанный к размеру свопчейна.
// Владелец — TextureManager. Проходы ссылаются на него КОСВЕННО (RenderPassTexturesInfo::shared_depth)
// и резолвят актуальный texture в момент begin прохода, поэтому ресайз = один вызов Resize(),
// без поимённого переназначения по проходам. Старая текстура уходит в отложенное удаление
// (живёт ещё BUFFERING_LEVEL кадров, пока её могут читать кадры in-flight).
struct SharedDepthTarget {
	SDL_GPUTexture* texture = nullptr;
	SDL_GPUTextureCreateInfo tci{};
	TextureManager* owner = nullptr;

	void Resize(uint32_t w, uint32_t h);
};