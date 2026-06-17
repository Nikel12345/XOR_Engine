#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <cstddef>
#include "ResourceManager.h"
#include "TextureData.h"

struct UploadTaskTexture {
	SDL_GPUTextureRegion dst{};
	std::vector<std::byte> pixels;          // BGRA32, плотно упакованные (декод один раз в TextureLoader)
	std::string name;                       // для диагностики при упаковке
	TextureHandle* target_handle;
	Uint32 offset;
	Uint32 size;
	Uint32 width, height, pitch;

};

namespace DefaultSamplersNames {
	inline constexpr const char* DEFAULT_SAMPLER = "_DefaultSampler";
	inline constexpr const char* DEFAULT_SHADOW_SAMPLER = "_DefaultShadowSampler";
	inline constexpr const char* VSM_SAMPLER = "_VsmSampler";
};

class TextureManager:public ResourceManager
{
public:
	TextureManager(SDL_GPUDevice* device);

	TextureAtlas* CreateTextureAtlas(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler);
	// �������� TextureAtlas �� ��� ������������ TextureAtlas
	// Create TextureAtlas from an already existing TextureAtlas
	// ������������ ��� �������� ������ � ������ ���������. �� ������ ����� GPU ��������, ���������� ��������� �� �������� � existing_atlas
	TextureAtlas* CreateTextureAtlas(const std::string& name, TextureAtlas* existing_atlas, SDL_GPUSampler* sampler);
	// Регистрирует текстуру из уже декодированных пикселей (BGRA32, width*height*4).
	// Загрузку с диска делает TextureLoader; оркестрация — в EngineContext.
	TextureHandle* CreateTexture(const std::string& name, const std::string& atlas_name, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);
	TextureHandle* CreateTexture(const std::string& name, TextureAtlas* atlas, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels);

	//// �������� ������ TextureData � ��������� �����������, ��� �������� ������ � ��������
	//// Create an empty TextureData with specified parameters, without uploading data to the texture
	//TextureData* CreateTextureData(const std::string& name, SDL_GPUTextureCreateInfo tci, SDL_GPUSampler* sampler);

	//// �������� TextureData �� ��� ������������ SDL_GPUTexture
	//// Create TextureData from an already existing SDL_GPUTexture
	//TextureData* CreateTextureData(const std::string& name, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

	// �������� ������ GPU ��������
	SDL_GPUTexture* CreateGPU_Texture(SDL_GPUTextureCreateInfo tci);

	void GenerateMipmaps(SDL_GPUCommandBuffer* cb);

	void ExecuteUploadTasks(SDL_GPUCopyPass* cp);
	SDL_GPUSampler* CreateSampler(const std::string& name, SDL_GPUSamplerCreateInfo sci);
	SDL_GPUSampler* GetSampler(const std::string& name);
	
	void DeleteTexture(const std::string& name);
	void DeleteTexture(SDL_GPUTexture* texture);
	~TextureManager();

	SDL_GPUTexture* main_pass_depth_texture = nullptr;

public:
	/*TextureData* GetTextureData(const std::string& name) {
		auto it = textures_data.find(name);
		if (it != textures_data.end()) {
			return it->second.get();
		}
		else {
			SDL_Log("Texture '%s' not found", name.c_str());
			return nullptr;
		}
	};*/
	TextureHandle* GetTextureHandle(const std::string& name) {
		auto it = handles_data.find(name);
		if (it != handles_data.end()) {
			return it->second.get();
		}
		else {
			SDL_Log("Texture '%s' not found", name.c_str());
			return nullptr;
		}
	};
	TextureAtlas* GetTextureAtlas(const std::string& name) {
		auto it = atlases_data.find(name);
		if (it != atlases_data.end()) {
			return it->second.get();
		}
		else {
			SDL_Log("Texture atlas '%s' not found", name.c_str());
			return nullptr;
		}
	};
private:
	void CreateUploadTask(TextureHandle* handle, uint32_t w, uint32_t h, std::vector<std::byte>&& pixels, const std::string& name);
	void _BuildUploadTasks();
	std::unordered_map<std::string, std::unique_ptr<TextureAtlas>> atlases_data;
	std::unordered_map<std::string, std::unique_ptr<TextureHandle>> handles_data;
	std::unordered_map<std::string, SDL_GPUSampler*> samplers_data;
	std::vector<UploadTaskTexture> upload_tasks;

};

