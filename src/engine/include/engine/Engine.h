#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "config.h"
#include "Aliases.h"

class QueueManager;
class TransferManager;
class BufferManager;
class TextureManager;
class ShaderManager;
class PipeManager;
class ModelManager;
class PassManager;
class ObjectManager;
class CameraManager;
class SlotController;
class ThreadController;
class MaterialManager;
class InputManager;
class TextureLoader;
class FontManager;
class BatchBuilder;
class PIB_DataModule;
class TransformDataModule;
class InstanceDataModule;
class LightDataModule;
class IndirectDataModule;
class BoundSphereDataModule;
class TextureStateDataModule;
class UI_DataModule;
class UI_Yoga;
class EngineContext;
struct TransferBufferData;
struct PrepassTimingReport;
struct ImDrawData;

struct EngineSizeState {
    std::atomic<uint64_t> window_size{ 0 };

    static uint64_t Pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
    static uint32_t W(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
    static uint32_t H(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }

    float WindowW() const { return static_cast<float>(W(window_size.load(std::memory_order_relaxed))); }
    float WindowH() const { return static_cast<float>(H(window_size.load(std::memory_order_relaxed))); }
};

struct EngineConfig {
    const char* title = "SDL_Engine";
    uint32_t width = 800;
    uint32_t height = 600;
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE;
    // ЖЕЛАЕМЫЕ: неподдержанные молча падают на VSYNC/SDR (устройство спрашивается в InitPlatform).
    SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_MAILBOX;
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    bool gpu_debug = true;
};

class Engine
{
public:
    explicit Engine(const EngineConfig& cfg);
    bool IsValid() const { return init_ok; }
    QueueManager* GetQueueManager() const { return queue_manager; }
    TransferManager* GetTransferManager() const { return transfer_manager; }
    BufferManager* GetBufferManager() const { return buffer_manager; }
    TextureManager* GetTextureManager() const { return texture_manager; }
    ShaderManager* GetShaderManager() const { return shader_manager; }
    PipeManager* GetPipeManager() const { return pipe_manager; }
    ModelManager* GetModelManager() const { return model_manager; }
    PassManager* GetPassManager() const { return pass_manager; }
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
    UI_DataModule* GetUIDataModule() const { return ui_data_module; }
    UI_Yoga* GetUIYoga() const { return ui_yoga; }


    void SaveScene(const SceneName& scene_name, const std::string& scenes_root);
    void LoadScene(const SceneName& scene_name, const std::string& scenes_root);

    void PrepareFunc(uint8_t idx);

    void UploadFunc(uint8_t slot);

    void ComputeFunc(uint8_t slot);

    bool RenderFunc(uint8_t idx);

    void FenceFunc(uint8_t slot);

    void BeginImGuiFrame();

    void EndImGuiFrame();

    // Колбэк зовёт SIM-поток. Задавать ДО Run().
    void SetGameIterate(std::function<void()> cb);

    // ОБЯЗАН зваться с main-потока: очередь сообщений окна привязана к потоку-создателю. Блокирует
    // до выхода; к возврату потоки конвейера уже остановлены и присоединены.
    int Run();

    // Можно с любого потока; насос заметит на следующей итерации.
    void RequestQuit() { running.store(false, std::memory_order_relaxed); }

    // Размер ОКНА и единственный размер, который движок знает. Во сколько пикселей рисуется сама
    // сцена, решает набор проходов, и знать это движку незачем: кому нужны настоящие пиксели —
    // берёт их с таргета, с которым работает.
    float GetWindowWidth()  const { return size_state.WindowW(); }
    float GetWindowHeight() const { return size_state.WindowH(); }

    // Звать с MAIN-потока. Публикует размер окна, и только его: таргеты пересоздаст гейт RenderFunc.
    void OnWindowResized(Sint32 window_w, Sint32 window_h);
    ~Engine();

private:
    bool InitPlatform(const EngineConfig& cfg);

    void PrepareFuncPrepassUndepended(uint8_t idx);
    void PrepareFuncPrepassDepended(uint8_t idx);

    void InitDefaultBufferUpdaters();
    void InitPasses();

    EngineSizeState size_state;

    // Рендер-поток стоит на всю загрузку сцены: компромисс «редактор читает живой ECS без замков»
    std::mutex scene_swap_mutex;

    SDL_Window* win = nullptr;
    SDL_GPUDevice* dev = nullptr;
    QueueManager* queue_manager = nullptr;
    TransferManager* transfer_manager = nullptr;
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
    FontManager* font_manager = nullptr;

    BatchBuilder* batch_builder = nullptr;

    PIB_DataModule* pib_data_module = nullptr;
    TransformDataModule* transform_data_module = nullptr;
    InstanceDataModule* instance_data_module = nullptr;
    LightDataModule* light_data_module = nullptr;
    IndirectDataModule* indirect_data_module = nullptr;
    BoundSphereDataModule* bound_sphere_data_module = nullptr;
    TextureStateDataModule* tex_state_data_module = nullptr;
    UI_DataModule* ui_data_module = nullptr;
    UI_Yoga* ui_yoga = nullptr;

    EngineContext* engine_context = nullptr;
    bool init_ok = false;
    std::atomic<bool> running{ false };
    ImDrawData* imgui_draw_data = nullptr;

    TransferBufferData* pending_upload_tbs[BUFFERING_LEVEL] = {};
    TransferBufferData* pending_texture_tbs[BUFFERING_LEVEL] = {};

    std::chrono::steady_clock::time_point last_frame_done_time{};
    bool last_frame_done_valid = false;
};
