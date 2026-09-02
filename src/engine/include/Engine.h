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

// ТОЛЬКО forward-декларации: все члены — указатели, геттеры отдают указатели, полные
// определения заголовку не нужны. Каждый cpp (движка и игры) сам инклюдит те менеджеры,
// которые реально зовёт — правка одного менеджер-заголовка больше НЕ пересобирает всех
// потребителей Engine.h (раньше это был хаб на весь мир: ~28 TU на любое изменение).
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

// Размерное состояние движка, разделяемое потоками. ДВЕ НЕЗАВИСИМЫЕ величины (упаковка (w<<32)|h —
// одна неделимая пара на атомик):
//   render_size — ВНУТРЕННЕЕ разрешение таргетов (scene_hdr/depth/bloom). Меняет SIM-поток —
//                 «полноценный ресайз» по кнопке игрового UI (команда). RENDER-поток читает и по
//                 изменению зовёт ExecuteResizeInstructions (пересоздание таргетов).
//   window_size — размер ОКНА. Меняет MAIN-поток (OnWindowResized, событие ОС). Свопчейн render-поток
//                 берёт из acquire, present-блит растягивает render→окно. Таргеты этим НЕ пересоздаются.
// Это разводит два разных события: смену окна (только презентация) и смену внутреннего разрешения игрой.
struct EngineSizeState {
    std::atomic<uint64_t> render_size{ 0 };   // ЖЕЛАЕМОЕ внутреннее разрешение (пишет sim)
    std::atomic<uint64_t> window_size{ 0 };   // размер окна (пишет main)
    uint64_t applied_render = 0;              // ПРИМЕНЁННОЕ render_size — «прошлое» гейта; трогает ТОЛЬКО render-поток

    static uint64_t Pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
    static uint32_t W(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
    static uint32_t H(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }

    // RENDER-поток: сравнивает ЖЕЛАЕМОЕ render_size со своим ПРОШЛЫМ (applied_render). Если изменилось —
    // отмечает применённым, отдаёт (w,h) и true (пора пересоздать таргеты). Иначе false. Гейт целиком тут.
    bool ConsumeRenderResize(uint32_t& w, uint32_t& h) {
        const uint64_t want = render_size.load(std::memory_order_acquire);
        if (want == applied_render || want == 0) return false;
        applied_render = want;
        w = W(want);  h = H(want);
        return true;
    }

    // Единый источник истины для размеров как float (UI-раскладка/камера/создание таргетов читают render,
    // презентация — window). Геттеры Engine делегируют СЮДА — отдельных размерных полей в движке нет.
    float RenderW() const { return static_cast<float>(W(render_size.load(std::memory_order_relaxed))); }
    float RenderH() const { return static_cast<float>(H(render_size.load(std::memory_order_relaxed))); }
    float WindowW() const { return static_cast<float>(W(window_size.load(std::memory_order_relaxed))); }
    float WindowH() const { return static_cast<float>(H(window_size.load(std::memory_order_relaxed))); }
};

// Всё, что игра вправе решать про окно и свопчейн. Остальное параметром не является:
// формат шейдеров (только SPIRV), число кадров в полёте (BUFFERING_LEVEL) и обход бага
// claim'а — это контракты движка, а не вкус игры, поэтому их тут нет (см. InitPlatform).
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
    // Поднимает платформу САМ (SDL_Init → окно → GPU-девайс → свопчейн) и владеет ею: dtor
    // рушит окно с девайсом и зовёт SDL_Quit. Отказ платформы — НЕ исключение: пишем в лог и
    // остаёмся невалидными (IsValid()==false, менеджеры не создавались), Run() сразу вернёт 1.
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


    // Сохранение/загрузка сцены-папки (dir): scene.json (ECS через om) + файлы ресурсов рядом
    // (tm/mm/sm — подключаются поэтапно). Публичная точка входа — ctx->Save/LoadScene (делегирует
    // сюда): оркестрация по менеджерам — забота Engine, а не контекста. Порядок load:
    // ресурсы (merge-upsert) → ECS (replace-on-load) → фикс-ап указателей → пересборка батчей.
    void SaveScene(const SceneName& scene_name, const std::string& dir);
    void LoadScene(const SceneName& scene_name, const std::string& dir);

    void PrepareFunc(uint8_t idx);

    void UploadFunc(uint8_t slot);

    // Вычислительная стадия конвейера. Пока ПУСТАЯ: слот просто проезжает сквозь неё.
    void ComputeFunc(uint8_t slot);

    bool RenderFunc(uint8_t idx);

    void FenceFunc(uint8_t slot);

    void BeginImGuiFrame();

    void EndImGuiFrame();

    // Игровой тик: зовётся SIM-потоком (ThreadController::SimulationThread), НЕ main-потоком.
    // Задавать до Run(): StartThreads внутри него уже раздаёт колбэки по потокам.
    void SetGameIterate(std::function<void()> cb);

    // Насос событий приложения. ОБЯЗАН зваться с main-потока — того, что инициализировал видео
    // и создал окно (очередь сообщений окна привязана к потоку-создателю). Поэтому вызов
    // блокирующий: движок не забирает поток себе, он лишь избавляет игру от переписывания цикла.
    // Возврат означает, что потоки конвейера УЖЕ остановлены и присоединены (см. тело).
    int Run();

    // Попросить цикл завершиться — можно с любого потока (например, кнопка «Выход» игрового UI
    // с sim-потока). Насос заметит на следующей итерации, максимум через SDL_Delay(16).
    void RequestQuit() { running.store(false, std::memory_order_relaxed); }

    // ВНУТРЕННЕЕ (render) разрешение как float для UI-раскладки/камеры/первичного создания таргетов —
    // читается ИЗ size_state_.render_size (единый источник истины, отдельных полей в движке нет). При
    // «полноценном» ресайзе игрой (SetRenderResolution) эти геттеры сразу отражают новое значение.
    float GetWidth()  const { return size_state_.RenderW(); }
    float GetHeight() const { return size_state_.RenderH(); }
    float GetWindowWidth()  const { return size_state_.WindowW(); }
    float GetWindowHeight() const { return size_state_.WindowH(); }

    // Событие ОС (MAIN-поток): публикует новый размер ОКНА. Только презентация (свопчейн+блит) —
    // таргеты НЕ пересоздаёт. render_w/h ПОКА не используются (смену внутреннего разрешения делает
    // SetRenderResolution из sim, а не событие окна) — оставлены под будущую симметрию.
    void OnWindowResized(Sint32 window_w, Sint32 window_h, Sint32 render_w, Sint32 render_h);
    // «Полноценный» ресайз (SIM-поток: игровой UI/команда) — сменить ВНУТРЕННЕЕ разрешение таргетов.
    // RENDER-поток подхватит по изменению и пересоздаст таргеты. ЗАГОТОВКА: пока никто не зовёт.
    void SetRenderResolution(uint32_t w, uint32_t h);
    ~Engine();

    const double targetUPS = 1000.0 / 60.0;
    const double targetFPS = 1000.0 / 60.0;

private:
    // SDL_Init + окно + GPU-девайс + claim + параметры свопчейна. Всё, что раньше руками писал
    // каждый main. false = дальше конструировать нечего (менеджеры не создаются).
    bool InitPlatform(const EngineConfig& cfg);

    void PrepareFuncPrepassUndepended(uint8_t idx);
    void PrepareFuncPrepassDepended(uint8_t idx);

	void InitDefaultBufferUpdaters();
    void InitPasses();
    void InitUICommands();
    // Движковые дефолтные текстуры (albedo/normal/orm/emissive) в _FallbackAtlas — чтобы редактор
    // мог создавать материалы и заполнять слоты по ролям без ассетов игры.
    void InitDefaultResources();

    // Движковый набор шейдеров (vs/fs/cs + render-программы штатных проходов) и их push-константы.
    // Всё с dont_save: сцена их не возит и не может потерять. Только СВОИ шейдеры сцена объявляет
    // в манифесте. Зовётся после InitDefaultResources — нужны пул, буферы и проходы.
    void InitDefaultShaders();

    PrepassTimingReport PrepareFuncPrepassDepended_Original(uint8_t slot);
    PrepassTimingReport PrepareFuncPrepassDepended_Optimized(uint8_t slot);

    // Размерное состояние движка ЦЕЛИКОМ (см. EngineSizeState): render_size (sim) / window_size (main) /
    // applied_render (render-локальный «прошлый»). Удаление текстур вправе делать ТОЛЬКО render-поток →
    // продьюсеры лишь ПУБЛИКУЮТ, а пересоздание таргетов исполняет RenderFunc по гейту ConsumeRenderResize.
    EngineSizeState size_state_;

    // Рендер-поток стоит на время загрузки сцены. НЕ противоречит компромиссу «редактор читает
    // живой ECS без замков» (см. RenderFunc): тот рассчитан на рваное ЗНАЧЕНИЕ — прочитать
    // матрицу в момент записи безвредно. LoadScene же РАЗРУШАЕТ сами структуры, по которым
    // редактор ходит: архетипы ECS, дерево UI_Yoga, словари менеджеров ресурсов (текстуры/модели/
    // материалы/шейдеры перезаливаются манифестами, а панели их перечисляют). Это use-after-free,
    // и окно тем шире, чем тяжелее сцена: на 1М снос архетипов и раздача заново — секунды,
    // рендер за это время успевает десятки кадров ВНУТРИ окна (на малой сцене — микросекунды,
    // отсюда «на маленьких не падает»).
    // Замок ВРЕМЕННЫЙ и грубый — держится весь кадр рендера и всю загрузку. Загрузка и так
    // означает ожидание, а в штатном кадре это незанятый lock/unlock. Настоящее закрытие —
    // UI-слепок или построение UI в sim-потоке (см. там же).
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
    bool init_ok = false;   // платформа поднялась и менеджеры созданы (см. IsValid)
    // Флаг насоса событий: пишет main-поток (закрытие окна) и любой другой через RequestQuit.
    std::atomic<bool> running{ false };
    ImDrawData* imgui_draw_data = nullptr;

    // Transfer-буферы, ушедшие в полёт для слота: держим до fence той фазы, что их читает
    // (контракт TransferManager::ReleaseTB). Стеш пишется ДО публикации fence, читается после
    // его сигнала — видимость между потоками даёт mutex SlotController'а.
    // Оба сабмитятся в PrepareFunc и оба отпускаются в UploadFunc, но буферами РАЗНЫМИ: заливка
    // буферов идёт на копировальную очередь, а текстурная (мипы и блиты превью — отрисовка,
    // копировальной их не исполнить) на графическую. Fences обоих лежат в SlotData::upload и
    // ждутся одним wait_all — отсюда и общая точка освобождения.
    TransferBufferData* pending_upload_tbs[BUFFERING_LEVEL] = {};
    TransferBufferData* pending_texture_tbs[BUFFERING_LEVEL] = {};

    // [PROFILE] Момент завершения предыдущего кадра (сигнал render-fence в FenceFunc).
    // Разница между соседними завершениями = реальный период кадра (1/период = FPS).
    // Трогает только FenceThread — синхронизация не нужна.
    std::chrono::steady_clock::time_point last_frame_done_time{};
    bool last_frame_done_valid = false;
};
