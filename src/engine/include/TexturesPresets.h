#pragma once
#include "SDL3/SDL.h"

enum class TexturePreset {
    Depth_FlatArray2048_1Layers,
    Depth_FlatArray2048_4Layers,
    Depth_FlatArray2048_8Layers,
    Depth_FlatArray2048_16Layers,
    Depth_FlatArray1024_8Layers,
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
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 8;
            info.num_levels = 1;
            break;

        case TexturePreset::Depth_CubemapArray2048_1Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 1 * 6; // 1 �������� * 6 ������
            info.num_levels = 1;

            break;

        case TexturePreset::Depth_CubemapArray2048_4Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 4 * 6; // 4 �������� * 6 ������
            info.num_levels = 1;

            break;

        case TexturePreset::Depth_CubemapArray2048_8Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 2048;
            info.height = 2048;
            info.layer_count_or_depth = 8 * 6; // 8 �������� * 6 ������
            info.num_levels = 1;

            break;
        case TexturePreset::Depth_CubemapArray1024_1Cubes:
            info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1 * 6; // 1 �������� * 6 ������
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
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
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
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
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
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
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
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            info.width = 1024;
            info.height = 1024;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            break;
        case TexturePreset::Albedo_Atlas4096_3Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = 4096;
            info.height = 4096;
            info.layer_count_or_depth = 3;
            info.num_levels = 3;
            break;
        case TexturePreset::Albedo_Atlas2048_1Layer:
            info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
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
            // ��� Custom ����� ��������� �������
            break;
        }

        return info;
    }

    // ── Атласы текстур материала (параметризуемые) ───────────────────────────────
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
        // Мип-генерация (SDL_GenerateMipmapsForGPUTexture) рендерит в уровни → требует COLOR_TARGET.
        // Добавляем его автоматически, когда мипы запрошены, иначе GenerateMipmaps молча ничего не даст.
        if (mip_levels > 1) usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
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
    // Albedo (sRGB-данные, но формат UNORM как везде в движке). COLOR_TARGET — может быть приёмником.
    inline SDL_GPUTextureCreateInfo AlbedoAtlas(uint32_t resolution, uint32_t layers = 1, uint32_t mip_levels = 1) {
        return _MaterialAtlas(resolution, layers, mip_levels,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
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

    // ������ � ����������� ��� ��������
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
    // (мипы = roughness-размытие, считаются от faceSize), COLOR_TARGET — чтобы GenerateMipmaps мог
    // в него писать. faceSize — единственный источник истины о разрешении env-куба.
    inline SDL_GPUTextureCreateInfo EnvCube(uint32_t faceSize) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_CUBE;
        info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = faceSize;
        info.height = faceSize;
        info.layer_count_or_depth = 6;
        uint32_t levels = 1;
        for (uint32_t s = faceSize; s > 1; s >>= 1) ++levels;
        info.num_levels = levels;
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
        // COLOR_TARGET — основной выход main-прохода; SAMPLER — для present-blit; STORAGE_READ/WRITE —
        // bloom-composite читает и пишет scene_hdr на месте (tonemap поверх + bloom).
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
                   | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
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
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // Уровень bloom-пирамиды: HDR, сэмплируется (LINEAR) при down/up-фильтрации И пишется/читается
    // как storage (down пишет, up прибавляет RMW). RGBA16F — безопасный storage-формат, цветная эмиссия.
    inline SDL_GPUTextureCreateInfo BloomLevel(uint32_t width, uint32_t height) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
                   | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.props = 0;
        return info;
    }

    // Вся bloom-пирамида в ОДНОЙ текстуре: уровень i = mip i (mip0 = ½ окна, далее /2). Down/up
    // читают mip-источник СЭМПЛЕРОМ (аппаратная билинейность) и storage-пишут соседний mip ТОЙ ЖЕ
    // текстуры в одном дисптаче — это требует флага SIMULTANEOUS_READ_WRITE (чтение+запись одной
    // текстуры в пределах одного compute-прохода; разные мипы не пересекаются → гонки нет). Поддержку
    // формата под SIMULTANEOUS надо проверять (SDL_GPUTextureSupportsFormat) — не универсальна.
    inline SDL_GPUTextureCreateInfo BloomPyramid(uint32_t width, uint32_t height, uint32_t levels) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER
                   | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                   | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = levels;
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
