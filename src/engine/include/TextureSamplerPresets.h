#pragma once
#include "SDL3/SDL.h"

enum class SamplerPreset {
	DEFAULT_SAMPLER,
	SHADOW_SAMPLER,
    VSM_SAMPLER,
    ENV_SAMPLER
};

namespace SamplerPresets {
    inline SDL_GPUSamplerCreateInfo GetSamplerCreateInfo(SamplerPreset preset) {
        SDL_GPUSamplerCreateInfo sci;
        SDL_zero(sci);

        switch (preset) {
        case SamplerPreset::DEFAULT_SAMPLER:
            sci.min_filter = SDL_GPU_FILTER_NEAREST;
            sci.mag_filter = SDL_GPU_FILTER_NEAREST;
            sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.mip_lod_bias = 0.0f;
            sci.max_anisotropy = 1.0f;
            sci.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            sci.min_lod = 0.0f;
            sci.max_lod = 32.0f;
            sci.enable_anisotropy = false;
            sci.enable_compare = false;
            sci.props = 0;
            break;

        case SamplerPreset::SHADOW_SAMPLER:
            sci.min_filter = SDL_GPU_FILTER_LINEAR;
            sci.mag_filter = SDL_GPU_FILTER_LINEAR;
            sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.enable_compare = true;
            sci.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            sci.mip_lod_bias = 0.0f;
            sci.min_lod = 0.0f;
            sci.max_lod = 0.0f;
            sci.enable_anisotropy = false;
            sci.max_anisotropy = 1.0f;
            sci.props = 0;
            break;

        case SamplerPreset::VSM_SAMPLER:
            sci.min_filter = SDL_GPU_FILTER_LINEAR;
            sci.mag_filter = SDL_GPU_FILTER_LINEAR;
            sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
            sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.enable_compare = false;
            sci.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            sci.mip_lod_bias = 0.0f;
            sci.min_lod = 0.0f;
            sci.max_lod = 32.0f;
            sci.enable_anisotropy = true;
            sci.max_anisotropy = 1.0f;
            sci.props = 0;
            break;

        case SamplerPreset::ENV_SAMPLER:
            // Отражение окружения: трилинейная фильтрация (мипы = roughness-размытие), clamp.
            sci.min_filter = SDL_GPU_FILTER_LINEAR;
            sci.mag_filter = SDL_GPU_FILTER_LINEAR;
            sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
            sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            sci.enable_compare = false;
            sci.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            sci.mip_lod_bias = 0.0f;
            sci.min_lod = 0.0f;
            sci.max_lod = 32.0f;
            sci.enable_anisotropy = false;
            sci.max_anisotropy = 1.0f;
            sci.props = 0;
            break;
        };

        return sci;
    };
};