#include "PCH.h"
#include "RenderCommandData.h"
#include "TextureData.h"

void RenderPassTexturesInfo::CreateColorTextureInfo(SDL_GPULoadOp load_op, SDL_GPUStoreOp store_op, SDL_FColor color, SDL_GPUTextureFormat format)
{
	SDL_GPUColorTargetInfo info{};
	info.load_op = load_op;
	info.store_op = store_op;
	info.clear_color = color;
	colorTargetInfos.push_back(info);
	color_formats.push_back(format);
	color_atlases.push_back(nullptr);   // параллельно colorTargetInfos; заполняет SetColorTexture
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
	if (index < color_atlases.size())
		color_atlases[index] = atlas;
	// Сбор флагов: ресурс сам копит роль в месте, где на него сослались (union, без приоритетов).
	if (atlas) atlas->debug_usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
}

void RenderPassTexturesInfo::SetDepthTexture(TextureAtlas* atlas)
{
	shared_depth = nullptr;
	depth_atlas = atlas;
	if (atlas) atlas->debug_usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
}

void RenderPassTexturesInfo::SetDepthTexture(SharedDepthTarget* dt)
{
	depth_atlas = nullptr;
	shared_depth = dt;
	if (dt) dt->debug_usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
}

void RenderPassStep::SetGlobalTextures(std::vector<TextureAtlas*> atlases)
{
	global_texture_bindings = std::move(atlases);
	// Роль однозначна: поле потребляется ровно одним SDL_BindGPUFragmentSamplers.
	for (TextureAtlas* atlas : global_texture_bindings)
		if (atlas) atlas->debug_usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
}

// Атласы/shared-depth → актуальные SDL-текстуры. Зовётся на ИСПОЛНЕНИИ (RenderPassStandardBody),
// а не на setup: GPU-текстуру таргета создаёт бейк (её ещё нет в момент объявления прохода) и
// подменяет ресайз. Держать копию SDL_GPUTexture* в проходе поэтому нельзя — протухнет.
void RenderPassTexturesInfo::ResolveTargets()
{
	const size_t n = color_atlases.size() < colorTargetInfos.size() ? color_atlases.size() : colorTargetInfos.size();
	for (size_t i = 0; i < n; ++i) {
		if (color_atlases[i])
			colorTargetInfos[i].texture = color_atlases[i]->texture_binding.texture;
	}

	if (shared_depth)     depthTargetInfo.texture = shared_depth->texture;
	else if (depth_atlas) depthTargetInfo.texture = depth_atlas->texture_binding.texture;
}