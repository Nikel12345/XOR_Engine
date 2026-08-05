#pragma once
#include "SDL3/SDL.h"

enum class TexturePreset {
    Depth_FlatArray2048_1Layers,
    Depth_FlatArray2048_4Layers,
    Depth_FlatArray2048_8Layers,
    Depth_FlatArray2048_16Layers,
    Depth_FlatArray1024_8Layers,
    Depth_FlatArray1024_16Layers,
    Depth_CubemapArray2048_1Cubes,
    Depth_CubemapArray2048_4Cubes,
    Depth_CubemapArray2048_8Cubes,
    Depth_CubemapArray1024_1Cubes,
	ShadowRG16_FlatArray1024_8Layers,
	ShadowRG16_FlatArray2048_8Layers,
    ShadowRG32_FlatArray1024_8Layers,
	ShadowRG32_FlatArray2048_8Layers,
    ShadowRGBA32_FlatArray1024_8Layers,
    ShadowRGBA32_FlatArray2048_8Layers,
    SingleDepth2048,
    TempShadowRG16_1024,
    TempShadowRG32_1024,
	TempShadowRG32_2048,
    TempShadowRGBA32_1024,
    TempShadowRGBA32_2048,
    TempDepth2048,
    TempDepth1024,
    Albedo_Atlas4096_3Layer,
	Albedo_Atlas2048_1Layer,
    NAOPBR_Atlas2048_1Layer,
    NAOPBR_Atlas4096_3Layer,
    // PBR-атласы по тирам детализации (все UNORM, как остальные — движок не использует sRGB-форматы).
    // Normal — высокодетальный (нужна попиксельная деталь); ORM/Emissive — низкочастотные → меньше.
    Normal_Atlas2048_1Layer,
    ORM_Atlas1024_1Layer,
    Emissive_Atlas1024_1Layer,
    Custom
};

namespace TexturePresets {
    // usage В ПРЕСЕТАХ = ТОЛЬКО НАМЕРЕНИЕ SAMPLER (атлас под текстуры материалов заводится пустым,
    // его цель иначе не узнать — см. TextureData.h). ВСЁ остальное CreateTextureAtlas СТРИЖЁТ:
    // роли таргетов/компьюта GPU-текстуре дают ДЕКЛАРАЦИИ (SetColorTexture/SetDepthTexture,
    // Create*Program, CreateBlitPass, материалы) до бейка. Флаги, оставшиеся в старых пресетах
    // ниже, — инертны.
    inline SDL_GPUTextureCreateInfo GetCreateInfo(TexturePreset preset) {
        SDL_GPUTextureCreateInfo info = {};
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;

        switch (preset) {
        case TexturePreset::Depth_FlatArray2048_1Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_FlatArray2048_4Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 6;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_FlatArray2048_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_FlatArray2048_16Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 16;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_FlatArray1024_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            // Без флагов: DEPTH — из SetDepthTexture теневых проходов, SAMPLER — из
            // SetGlobalTextures main-прохода (декларации).
            info.usage = 0;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_FlatArray1024_16Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 16;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_CubemapArray2048_1Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1 * 6;
            info.num_levels = 1;

            break;

        case TexturePreset::Depth_CubemapArray2048_4Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 4 * 6;
            info.num_levels = 1;

            break;

        case TexturePreset::Depth_CubemapArray2048_8Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8 * 6;
            info.num_levels = 1;

            break;
        case TexturePreset::Depth_CubemapArray1024_1Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1 * 6;
            info.num_levels = 1;
            break;

		case TexturePreset::ShadowRG16_FlatArray1024_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
			break;

		case TexturePreset::ShadowRG16_FlatArray2048_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::ShadowRG32_FlatArray1024_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
            info.usage = 0;   // роли выведут декларации (проход/компьют/глобальный бинд)
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

		case TexturePreset::ShadowRG32_FlatArray2048_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
			break;

        case TexturePreset::ShadowRGBA32_FlatArray1024_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::ShadowRGBA32_FlatArray2048_8Layers:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::SingleDepth2048:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            // Без флагов: DEPTH_STENCIL_TARGET доложит SetDepthTexture прохода (декларация).
            info.usage = 0;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

		case TexturePreset::TempShadowRG32_2048:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
			info.num_levels = 1;
			break;

        case TexturePreset::TempShadowRG16_1024:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::TempShadowRG32_1024:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
            info.usage = 0;   // роли выведут декларации (проход/компьют/глобальный бинд)
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::TempShadowRGBA32_1024:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::TempShadowRGBA32_2048:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::TempDepth2048:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::TempDepth1024:
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = 0;   // DEPTH — из SetDepthTexture прохода (декларация)
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;
        case TexturePreset::Albedo_Atlas4096_3Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;   // намерение; COLOR_TARGET при мипах даст мип-правило
            info.width = 4096;
            info.height = 4096;
            info.layer_count_or_depth = 3;
            info.num_levels = 3;
            break;
        case TexturePreset::Albedo_Atlas2048_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;   // намерение; COLOR_TARGET при мипах даст мип-правило
			info.width = 2048;
			info.height = 2048;
			info.layer_count_or_depth = 1;
            info.num_levels = 1;
			break;
        case TexturePreset::NAOPBR_Atlas2048_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;
        case TexturePreset::NAOPBR_Atlas4096_3Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 4096;
            info.height = 4096;
            info.layer_count_or_depth = 3;
            info.num_levels = 1;
            break;

        case TexturePreset::Normal_Atlas2048_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;   // линейные данные (нормали)
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;
        case TexturePreset::ORM_Atlas1024_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;   // R=AO G=Rough B=Metal (линейные)
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;
        case TexturePreset::Emissive_Atlas1024_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;

        case TexturePreset::Custom:
            break;
        }

        return info;
    }

    // Квадратный 2D-array атлас под material-текстуры: BGRA8 UNORM (движок не использует
    // sRGB-форматы), задаёшь разрешение, число слоёв и число мип-уровней.
    // Полная мип-цепочка от разрешения: log2(res)+1. Для partial-цепочки передавай число вручную.
    inline uint32_t FullMipLevels(uint32_t resolution) {
        uint32_t levels = 1;
        for (uint32_t s = resolution; s > 1; s >>= 1) ++levels;
        return levels;
    }
    inline SDL_GPUTextureCreateInfo _MaterialAtlas(uint32_t resolution, uint32_t layers,
                                                   uint32_t mip_levels, SDL_GPUTextureUsageFlags usage) {
        // usage здесь = НАМЕРЕНИЕ (SAMPLER). Мип-правило (num_levels>1 → SAMPLER|COLOR_TARGET,
        // мип-ген требует оба — зонд) применяет сам CreateTextureAtlas, дублировать не нужно.
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        info.usage = usage;
        info.width = resolution;
        info.height = resolution;
        info.layer_count_or_depth = layers;
        info.num_levels = mip_levels < 1 ? 1 : mip_levels;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }
    // Albedo (sRGB-данные, но формат UNORM как везде в движке).
    // COLOR_TARGET здесь НЕ ставим: его добавит _MaterialAtlas ровно тогда, когда запрошены мипы
    // (мип-ген рендерит в уровни). Безусловный COLOR_TARGET раздавал его и немипованным атласам —
    // например _FallbackAtlas (64px, 1 мип), которому мип-ген не нужен вовсе.
    inline SDL_GPUTextureCreateInfo AlbedoAtlas(uint32_t resolution, uint32_t layers = 1, uint32_t mip_levels = 1) {
        return _MaterialAtlas(resolution, layers, mip_levels, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }
    // Normal — линейные данные (tangent-space нормали).
    inline SDL_GPUTextureCreateInfo NormalAtlas(uint32_t resolution, uint32_t layers = 1, uint32_t mip_levels = 1) {
        return _MaterialAtlas(resolution, layers, mip_levels, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }
    // ORM — упаковка R=AO, G=Roughness, B=Metallic (линейные).
    inline SDL_GPUTextureCreateInfo ORMAtlas(uint32_t resolution, uint32_t layers = 1, uint32_t mip_levels = 1) {
        return _MaterialAtlas(resolution, layers, mip_levels, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }
    // Emissive — цвет свечения.
    inline SDL_GPUTextureCreateInfo EmissiveAtlas(uint32_t resolution, uint32_t layers = 1, uint32_t mip_levels = 1) {
        return _MaterialAtlas(resolution, layers, mip_levels, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }

    inline SDL_GPUTextureCreateInfo ShadowCubemapArray(uint32_t num_cubemaps, uint32_t resolution = 2048) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = resolution;
        info.height = resolution;
        info.layer_count_or_depth = num_cubemaps * 6;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // Env-кубмап для отражений металла: квадратные грани faceSize×faceSize, полная мип-цепочка
    // (мипы = roughness-размытие, считаются от faceSize). SAMPLER|COLOR_TARGET даст мип-правило
    // CreateTextureAtlas (num_levels>1). faceSize — единственный источник истины о разрешении.
    inline SDL_GPUTextureCreateInfo EnvCube(uint32_t faceSize) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_CUBE;
        info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        info.usage = 0;
        info.width = faceSize;
        info.height = faceSize;
        info.layer_count_or_depth = 6;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // HDR-таргет сцены (location 0 main-прохода): линейный цвет ДО тонмаппинга, эмиссия и блики
    // уходят за 1.0. SAMPLER — чтобы финальный blit/постпроцесс мог читать. На экран выводится
    // отдельным present-проходом (формат свопчейна 8-бит, поэтому прямой рендер сюда невозможен).
    inline SDL_GPUTextureCreateInfo SceneHDR(uint32_t width, uint32_t height) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        // Без флагов — все роли выводятся из деклараций: COLOR_TARGET — SetColorTexture main-прохода;
        // SAMPLER — present-blit (CreateBlitPass src) и bloom-prefilter (сэмплер compute-sp);
        // COMPUTE_STORAGE_WRITE — rw-биндинг bloom-composite.
        info.usage = 0;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // MRT-таргет эмиссии (location 1 main-прохода): цветная эмиссия в HDR — источник для bloom.
    // R11G11B10 = 3 канала, 4 байта (вдвое дешевле RGBA16F); альфа эмиссии не нужна.
    inline SDL_GPUTextureCreateInfo EmissionHDR(uint32_t width, uint32_t height) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
        info.usage = 0;   // COLOR_TARGET — MRT main-прохода, SAMPLER — bloom-prefilter (декларации)
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // ОДИН уровень bloom-пирамиды — ОТДЕЛЬНАЯ текстура (уровень 0 = ½ окна, дальше /2), а не мип
    // общей. Раньше пирамида была одной текстурой с мипами, но down/up-шаг биндит dst-мип как
    // RW-storage, а src читает сэмплером — sampled-вью покрывает ВСЕ мипы, включая RW-мип
    // (GENERAL), и дескриптор нарушает layout-правила (VUID-VkDescriptorImageInfo-imageLayout-00344).
    // Отдельные текстуры исключают алиасинг по построению.
    // Формат согласован с [[vk::image_format("rgba16f")]] в bloom-шейдерах.
    //
    // Без флагов — их выводят декларации bloom-программ: SAMPLER/COMPUTE_STORAGE_WRITE из
    // сэмплер/rw-списков, SIMULTANEOUS — из тега need_simultaneous на rw-биндинге bloom_up
    // (его получают только НАЗНАЧЕНИЯ апсемпла; самый мелкий уровень — лишь источник).
    inline SDL_GPUTextureCreateInfo BloomLevel(uint32_t width, uint32_t height) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = 0;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    inline SDL_GPUTextureCreateInfo ShadowDirectionalArray(uint32_t num_lights, uint32_t resolution = 2048) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = resolution;
        info.height = resolution;
        info.layer_count_or_depth = num_lights;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }
}
