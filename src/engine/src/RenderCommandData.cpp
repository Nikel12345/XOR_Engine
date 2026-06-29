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

void RenderPassTexturesInfo::SetColorTexture(SDL_GPUTexture* tex, uint32_t index)
{
	if (index < colorTargetInfos.size())
		colorTargetInfos[index].texture = tex;
}

void RenderPassTexturesInfo::SetDepthTexture(SDL_GPUTexture* tex)
{
	if (depthTargetInfo.texture) {

	}
	shared_depth = nullptr;
	depthTargetInfo.texture = tex;
}

void RenderPassTexturesInfo::SetDepthTexture(SharedDepthTarget* dt)
{
	shared_depth = dt;
	depthTargetInfo.texture = dt ? dt->texture : nullptr;
}