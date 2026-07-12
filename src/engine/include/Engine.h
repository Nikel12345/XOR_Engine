#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>  // bind_shader_functions — колбэк пере-привязки push после LoadScene
#include <string>
#include "config.h"    // BUFFERING_LEVEL — размер pending_upload_tbs
#include "Aliases.h"   // SceneName

// ТОЛЬКО forward-декларации: все члены — указатели, геттеры отдают указатели, полные
// определения заголовку не нужны. Каждый cpp (движка и игры) сам инклюдит те менеджеры,
// которые реально зовёт — правка одного менеджер-заголовка больше НЕ пересобирает всех
// потребителей Engine.h (раньше это был хаб на весь мир: ~28 TU на любое изменение).
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
class BatchBuilder;
class PIB_DataModule;
class TransformDataModule;
class InstanceDataModule;
class LightDataModule;
class IndirectDataModule;
class BoundSphereDataModule;
class EngineContext;
struct TransferBufferData;
struct PrepassTimingReport;
struct ImDrawData;

class Engine
{
public:
    Engine(SDL_Window* window, SDL_GPUDevice* dev, float width, float height);
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


    // Сохранение/загрузка сцены-папки (dir): scene.json (ECS через om) + файлы ресурсов рядом
    // (tm/mm/sm — подключаются поэтапно). Публичная точка входа — ctx->Save/LoadScene (делегирует
    // сюда): оркестрация по менеджерам — забота Engine, а не контекста. Порядок load:
    // ресурсы (merge-upsert) → ECS (replace-on-load) → фикс-ап указателей → пересборка батчей.
    void SaveScene(const SceneName& scene_name, const std::string& dir);
    void LoadScene(const SceneName& scene_name, const std::string& dir);

    // Колбэк пере-привязки код-байндингов (push_func и т.п.) к sp ПОСЛЕ каждой загрузки сцены.
    // Регистрирует игра (у неё живут лямбды), зовёт LoadScene в конце: код-байндинги не
    // сериализуются, а перенос со старой sp по имени ломался бы на переименовании.
    void SetBindShaderFunctions(std::function<void()> fn) { bind_shader_functions = std::move(fn); }

    //void Iterate();
    void PrepareFunc(uint8_t idx);
    // Сверка ручных usage-флагов с авто-собранными печатается один раз, после первого бейка.
    bool usage_report_done = false;

    void UploadFunc(uint8_t slot);

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
    // Билтин-типы material-params (None/Opaque/Transparent) в реестр для UI-дропдауна Kind.
    void InitDefaultMaterialParams();
    // Движковые дефолтные текстуры (albedo/normal/orm/emissive) в _FallbackAtlas — чтобы редактор
    // мог создавать материалы и заполнять слоты по ролям без ассетов игры.
    void InitDefaultResources();

    PrepassTimingReport PrepareFuncPrepassDepended_Original(uint8_t slot);
    PrepassTimingReport PrepareFuncPrepassDepended_Optimized(uint8_t slot);

    float width;
    float height;

    SDL_Window* win = nullptr;
    SDL_GPUDevice* dev = nullptr;
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

    BatchBuilder* batch_builder = nullptr;

	PIB_DataModule* pib_data_module = nullptr;
	TransformDataModule* transform_data_module = nullptr;
	InstanceDataModule* instance_data_module = nullptr;
	LightDataModule* light_data_module = nullptr;
	IndirectDataModule* indirect_data_module = nullptr;
    BoundSphereDataModule* bound_sphere_data_module = nullptr;

	EngineContext* engine_context;
	std::function<void()> bind_shader_functions;   // см. SetBindShaderFunctions
    std::atomic<bool> running = true;
    ImDrawData* imgui_draw_data = nullptr;

    // Transfer-буферы upload'а, ушедшего в полёт для слота. Вернутся в пул на
    // UploadThread (Engine::UploadFunc) после сигнала upload-fence. Видимость
    // между потоками обеспечивает mutex SlotController'а: стеш пишется ДО
    // SetSlotState(UPLOADING), читается после проверки IsUploadingSlot.
    struct PendingUploadTBs {
        TransferBufferData* buffers_tbd = nullptr;
        TransferBufferData* textures_tbd = nullptr;
    };
    PendingUploadTBs pending_upload_tbs[BUFFERING_LEVEL];

    // [PROFILE] Момент завершения предыдущего кадра (сигнал render-fence в FenceFunc).
    // Разница между соседними завершениями = реальный период кадра (1/период = FPS).
    // Трогает только FenceThread — синхронизация не нужна.
    std::chrono::steady_clock::time_point last_frame_done_time{};
    bool last_frame_done_valid = false;
};
