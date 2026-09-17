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
#include "GraphicsConfig.h"   // по значению внутри TargetSizeInputs — forward-декларацией не обойтись

// ТОЛЬКО forward-декларации: полный заголовок инклюдит тот cpp, который реально зовёт менеджер.
// Иначе правка любого из них пересобирает всех потребителей Engine.h.
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

// Размер ОКНА: пишет MAIN-поток из события ОС, читают sim и render. Внутреннее разрешение здесь
// НЕ хранится — оно производное от конфига и этого размера, и второй источник истины разошёлся бы.
// Упаковка (w<<32)|h — чтобы пара менялась одним атомарным словом и не рвалась пополам.
struct EngineSizeState {
    std::atomic<uint64_t> window_size{ 0 };

    static uint64_t Pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
    static uint32_t W(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
    static uint32_t H(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }

    float WindowW() const { return static_cast<float>(W(window_size.load(std::memory_order_relaxed))); }
    float WindowH() const { return static_cast<float>(H(window_size.load(std::memory_order_relaxed))); }
};

// Входы, от которых зависят размеры экранных таргетов. Сравнивается СНИМОК целиком, а не счётчик
// ревизий: ревизия делает ошибку липкой — совпала, и повода пересчитать больше нет, даже если
// применено было не то. Сравнение сгенерированное, не memcmp: тот прочитал бы и байты выравнивания.
struct TargetSizeInputs {
    GraphicsConfig cfg{};
    uint32_t out_w = 0;   // размер НАЗНАЧЕНИЯ (свопчейн); сейчас это окно, у вида редактора будет панель
    uint32_t out_h = 0;
    bool operator==(const TargetSizeInputs&) const = default;
};

// Всё, что игра вправе решать про окно и свопчейн. Формат шейдеров, число кадров в полёте и обход
// бага claim'а сюда не входят — это контракты движка (см. InitPlatform).
struct EngineConfig {
    const char* title = "SDL_Engine";
    uint32_t width = 800;
    uint32_t height = 600;
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE;
    // ЖЕЛАЕМЫЕ: неподдержанные молча падают на VSYNC/SDR (устройство спрашивается в InitPlatform).
    SDL_GPUPresentMode present_mode = SDL_GPU_PRESENTMODE_MAILBOX;
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    bool gpu_debug = true;
    // Только СТАРТОВЫЕ: движок кладёт копию на кучу, дальше её правят через GetGraphicsConfig().
    GraphicsConfig graphics{};
};

class Engine
{
public:
    // Отказ платформы — не исключение: движок остаётся невалидным (менеджеры не создавались),
    // и Run() сразу вернёт 1.
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

    // Колбэк зовёт SIM-поток. Задавать ДО Run(): он уже раздаёт колбэки по потокам.
    void SetGameIterate(std::function<void()> cb);

    // ОБЯЗАН зваться с main-потока: очередь сообщений окна привязана к потоку-создателю. Блокирует
    // до выхода; к возврату потоки конвейера уже остановлены и присоединены.
    int Run();

    // Можно с любого потока; насос заметит на следующей итерации.
    void RequestQuit() { running.store(false, std::memory_order_relaxed); }

    // ВНУТРЕННЕЕ разрешение, и это ЗАПРОС: после правки конфига оно опережает реальные размеры
    // таргетов на кадр. Там, где расхождение значимо (создание таргета, пиксельные координаты),
    // годятся только применённые размеры — их знает render-поток. Раскладка UI берёт Window-пару:
    // размер кнопки задан относительно экрана, а не частоты сэмплирования.
    float GetWidth()  const { uint32_t w, h; ComputeRenderSize(w, h); return static_cast<float>(w); }
    float GetHeight() const { uint32_t w, h; ComputeRenderSize(w, h); return static_cast<float>(h); }
    float GetWindowWidth()  const { return size_state.WindowW(); }
    float GetWindowHeight() const { return size_state.WindowH(); }

    // Живой указатель: правка полей на месте и есть способ менять настройки, гейт RenderFunc
    // подхватит её сам.
    GraphicsConfig* GetGraphicsConfig() const { return graphics_config; }

    // Звать с MAIN-потока. Публикует размер окна, и только его: таргеты пересоздаст гейт RenderFunc.
    void OnWindowResized(Sint32 window_w, Sint32 window_h);
    ~Engine();

private:
    bool InitPlatform(const EngineConfig& cfg);

    void PrepareFuncPrepassUndepended(uint8_t idx);
    void PrepareFuncPrepassDepended(uint8_t idx);

    void InitDefaultBufferUpdaters();
    void InitPasses();

    // Пересоздание таргетов исполняет RenderFunc, а не тот, кто поменял размер: удаление текстур
    // вправе делать только render-поток.
    EngineSizeState size_state;

    void ComputeRenderSize(uint32_t& w, uint32_t& h) const {
        const uint64_t win = size_state.window_size.load(std::memory_order_relaxed);
        GfxRenderTarget(*graphics_config, EngineSizeState::W(win), EngineSizeState::H(win), w, h);
    }

    GraphicsConfig* graphics_config = nullptr;
    // Снимок входов, под которые таргеты уже пересозданы. Трогает ТОЛЬКО render-поток.
    TargetSizeInputs applied_inputs{};

    // Рендер-поток стоит на всю загрузку сцены: компромисс «редактор читает живой ECS без замков»
    // рассчитан на рваное ЗНАЧЕНИЕ, а загрузка разрушает сами структуры, по которым ходят панели, —
    // архетипы, дерево UI и реестры менеджеров. Держится весь кадр рендера и всю загрузку.
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

    // Две очереди, поэтому и две пачки: буферы заливает копировальная, текстуры — графическая
    // (мипы и блиты превью копировальной не исполнить). Фенсы обоих ждутся одним wait_all.
    TransferBufferData* pending_upload_tbs[BUFFERING_LEVEL] = {};
    TransferBufferData* pending_texture_tbs[BUFFERING_LEVEL] = {};

    // Трогает только FenceThread.
    std::chrono::steady_clock::time_point last_frame_done_time{};
    bool last_frame_done_valid = false;
};
