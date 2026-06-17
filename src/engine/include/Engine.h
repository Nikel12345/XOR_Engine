#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <iostream>
#include <string_view>
#include <thread>
#include "BufferManager.h"
#include "TextureManager.h"
#include "ShaderManager.h"
#include "PipeManager.h"
#include "ModelManager.h"
#include "RenderManager.h"
#include "ObjectManager.h"
//#include "PositionStructure.h"
#include "CameraManager.h"
#include "SlotController.h"
#include "ThreadController.h"
#include "LightStruct.h"
#include "PIB_DataModule.h"
#include "TransformDataModule.h"
#include "LightDataModule.h"
#include "MaterialManager.h"
#include "BatchBuilder.h"
#include "DefaultUpdateSet.h"
#include "DefaultRenderPassSet.h"
#include "IndirectDataModule.h"
#include "BoundSphereDataModule.h"
#include "CountBufferDataModule.h"
#include "config.h"
#include "UI_ImGui.h"
#include "EngineContext.h"
#include "InputManager.h"
#include "TextureLoader.h"

struct PrepassTimingReport;
static bool UPS_priority = true;
class Engine
{
public:
    Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height);
    BufferManager* GetBufferManager() const { return buffer_manager; }
    TextureManager* GetTextureManager() const { return texture_manager; }
    ShaderManager* GetShaderManager() const { return shader_manager; }
    PipeManager* GetPipeManager() const { return pipe_manager; }
    ModelManager* GetModelManager() const { return model_manager; }
    PassManager* GetRenderManager() const { return pass_manager; }
	ObjectManager* GetObjectManager() const { return object_manager; }
	CameraManager* GetCameraManager() const { return camera_manager; }
	MaterialManager* GetMaterialManager() const { return material_manager; }
    BatchBuilder* GetBatchBuilder() const { return batch_builder; }
    
	EngineContext* GetEngineContext() { return engine_context; }

	ThreadController* GetThreadController() const { return thread_controller; }

	InputManager* GetInputManager() const { return input_manager; }

	PIB_DataModule* GetPIBDataModule() const { return pib_data_module; }
	TransformDataModule* GetTransformDataModule() const { return transform_data_module; }
	LightDataModule* GetLightDataModule() const { return light_data_module; }


    //void Iterate();
    void PrepareFunc(uint8_t idx);

	void UploadFunc(uint8_t idx);
    bool RenderFunc(uint8_t idx);

    void FenceFunc(uint8_t slot);

    void BeginImGuiFrame();

    void EndImGuiFrame();

	//void SetFrameIndex(uint8_t idx) { frame_index.store(idx); }
    //uint8_t GetFrameIndex() const { return frame_index.load(); }

    float GetWidth()  const { return width; }
    float GetHeight() const { return height; }
    void OnWindowResized(Sint32 w, Sint32 h);
    ~Engine();

    const double targetUPS = 1000.0 / 60.0;
    const double targetFPS = 1000.0 / 60.0;

private:
    void PrepareFuncPrepassUndepended(uint8_t idx);
    void PrepareFuncPrepassDepended(uint8_t idx);

	void InitDefaultBufferUpdaters();
    void InitPasses();
    void InitUICommands();

    PrepassTimingReport PrepareFuncPrepassDepended_Original(uint8_t slot);
    PrepassTimingReport PrepareFuncPrepassDepended_Optimized(uint8_t slot);

    float width;
    float height;

    SDL_Window* win = nullptr;
    SDL_GPUDevice* dev = nullptr;
    BufferManager* buffer_manager = nullptr;
    TextureManager* texture_manager = nullptr;
    ShaderManager* shader_manager = nullptr;
    PipeManager* pipe_manager = nullptr;
    ModelManager* model_manager = nullptr;
    PassManager* pass_manager = nullptr;
    ObjectManager* object_manager = nullptr;
    CameraManager* camera_manager = nullptr;
	SlotController* slot_controller = nullptr;
    ThreadController* thread_controller = nullptr;
	MaterialManager* material_manager = nullptr;
	InputManager* input_manager = nullptr;
	TextureLoader* texture_loader = nullptr;

    BatchBuilder* batch_builder = nullptr;

	PIB_DataModule* pib_data_module = nullptr;
	TransformDataModule* transform_data_module = nullptr;
	LightDataModule* light_data_module = nullptr;
	IndirectDataModule* indirect_data_module = nullptr;
    BoundSphereDataModule* bound_sphere_data_module = nullptr;
    CountBufferDataModule* count_data_module = nullptr;

	EngineContext* engine_context;
    std::atomic<bool> running = true;
    ImDrawData* imgui_draw_data = nullptr;


};


