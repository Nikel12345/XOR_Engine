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

        case TexturePreset::Custom:
            // ��� Custom ����� ��������� �������
            break;
        }

        return info;
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
