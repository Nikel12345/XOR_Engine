#include "PCH.h"
#include "RenderCommandData.h"
#include "TextureData.h"

void RenderPassTexturesInfo::CreateColorTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_FColor color, SDL_GPUTextureFormat format)
{
	if (color_targets.size() >= MAX_COLOR_TARGETS) {
		SDL_Log("CreateColorTextureInfo: pass already has %u color targets - SDL takes no more, target ignored",
			MAX_COLOR_TARGETS);
		return;
	}
	ColorTarget target{};
	target.info.load_op = load_op;
	target.info.store_op = store_op;
	target.info.clear_color = color;
	target.format = format;
	color_targets.push_back(target);
}

uint32_t RenderPassTexturesInfo::CollectColorTargetInfos(SDL_GPUColorTargetInfo* out, uint32_t capacity) const
{
	const uint32_t n = std::min(safe_u32(color_targets.size()), capacity);
	for (uint32_t i = 0; i < n; ++i) out[i] = color_targets[i].info;
	return n;
}

void RenderPassTexturesInfo::CreateDepthTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_GPUTextureFormat format)
{
	depthTargetInfo.clear_depth = 1.0f;
	depthTargetInfo.clear_stencil = 0;
	depthTargetInfo.load_op = load_op;
	depthTargetInfo.store_op = store_op;
	depthTargetInfo.cycle = false;
	depthTargetInfo.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
	depthTargetInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
	depth_format = format;
}

void RenderPassTexturesInfo::SetColorTexture(TextureAtlas* atlas, uint32_t index)
{
	if (index < color_targets.size())
		color_targets[index].atlas = atlas;
	if (atlas) atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
}

void RenderPassTexturesInfo::SetDepthTexture(TextureAtlas* atlas)
{
	depth_atlas = atlas;
	if (atlas) atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
}

void RenderPassStep::SetGlobalTextures(std::vector<TextureAtlas*> atlases)
{
	global_texture_bindings = std::move(atlases);
	for (TextureAtlas* atlas : global_texture_bindings)
		if (atlas) atlas->tci.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
}

void RenderPassTexturesInfo::ResolveTargets()
{
	for (ColorTarget& target : color_targets) {
		if (target.atlas)
			target.info.texture = target.atlas->texture_binding.texture;
	}

	if (depth_atlas) depthTargetInfo.texture = depth_atlas->texture_binding.texture;
}