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

// Единственный размер, который движок хранит и публикует между потоками: размер ОКНА — свойство
// платформы (пишет MAIN-поток из события ОС, читают sim и render).
// Внутреннего разрешения здесь НЕТ намеренно: оно производное от GraphicsConfig и этого размера,
// и хранить его отдельно значило бы завести второй источник истины про одно и то же — он обязательно
// разойдётся с настройками. Выводят его Engine::GetWidth/GetHeight прямо на месте.
// Упаковка (w<<32)|h — чтобы пара менялась одним атомарным словом и не рвалась пополам.
struct EngineSizeState {
    std::atomic<uint64_t> window_size{ 0 };

    static uint64_t Pack(uint32_t w, uint32_t h) { return (static_cast<uint64_t>(w) << 32) | h; }
    static uint32_t W(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
    static uint32_t H(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }

    float WindowW() const { return static_cast<float>(W(window_size.load(std::memory_order_relaxed))); }
    float WindowH() const { return static_cast<float>(H(window_size.load(std::memory_order_relaxed))); }
};

// Всё, от чего зависят размеры экранных таргетов. Гейт RenderFunc сравнивает это ЦЕЛИКОМ: любое поле
// конфига меняет вывод размеров ровно так же, как смена окна, и разделять два источника незачем.
// Сравнение — сгенерированное компилятором, а не memcmp: memcmp прочитал бы ещё и байты выравнивания.
//
// Почему сравнение снимка, а не счётчик ревизий: ревизия делает ошибку ЛИПКОЙ. Совпала — и повода
// пересчитать больше нет, даже если применено было не то. Снимок же расходится снова на следующем
// кадре и сам себя чинит; заодно окно, внутреннее разрешение и конфиг идут одним путём.
struct TargetSizeInputs {
    GraphicsConfig cfg{};
    uint32_t out_w = 0;   // размер НАЗНАЧЕНИЯ (свопчейн); сейчас это окно, у вида редактора будет панель
    uint32_t out_h = 0;
    bool operator==(const TargetSizeInputs&) const = default;
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
    // Стартовые настройки графики — игра задаёт их здесь, а не правкой дефолтов в GraphicsConfig.h.
    // Дальше они живут своей жизнью: движок кладёт копию на кучу, и менять их можно в рантайме
    // через GetGraphicsConfig() (см. GraphicsConfig — владение и поток).
    GraphicsConfig graphics{};
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


    // Сохранение/загрузка сцены-папки: scene.json (ECS через om) + файлы ресурсов рядом
    // (tm/mm/sm — подключаются поэтапно). Папка сцены = scenes_root/scene_name (имя папки И ЕСТЬ
    // имя сцены, см. kScenesRoot) — путь складывается тут, чтобы список папок в редакторе был
    // готовым списком имён сцен. Публичная точка входа — ctx->Save/LoadScene (делегирует сюда):
    // оркестрация по менеджерам — забота Engine, а не контекста. Порядок load:
    // ресурсы (merge-upsert) → ECS (replace-on-load) → фикс-ап указателей → пересборка батчей.
    void SaveScene(const SceneName& scene_name, const std::string& scenes_root);
    void LoadScene(const SceneName& scene_name, const std::string& scenes_root);

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

    // ВНУТРЕННЕЕ (render) разрешение: выводится на месте из GraphicsConfig и размера окна — теми же
    // функциями, что и размеры таргетов, поэтому разойтись с ними не может.
    //
    // Это ЗАПРОС, а не факт: сразу после правки конфига он опережает реальные размеры таргетов на
    // один кадр (их пересоздаст гейт RenderFunc). Единственный потребитель — отношение сторон камеры,
    // и оно от расхождения не страдает: обе стороны считаются одной формулой, так что их отношение
    // у запроса и у факта совпадает всегда.
    // Для того, где расхождение значимо — создание таргета, пересчёт пиксельных координат, — эта пара
    // НЕ годится: применённые размеры знает только render-поток (applied_inputs_).
    //
    // Раскладка UI берёт НЕ это, а Window-пару: размер кнопки в пикселях задан относительно экрана,
    // а не частоты сэмплирования (иначе при render != window весь UI съезжает в масштабе).
    float GetWidth()  const { uint32_t w, h; ComputeRenderSize(w, h); return static_cast<float>(w); }
    float GetHeight() const { uint32_t w, h; ComputeRenderSize(w, h); return static_cast<float>(h); }
    float GetWindowWidth()  const { return size_state_.WindowW(); }
    float GetWindowHeight() const { return size_state_.WindowH(); }

    // Настройки графики. Живой указатель, а не копия: правка полей на месте — и есть способ их
    // менять (см. GraphicsConfig — владение и поток). Гейт RenderFunc подхватит изменение сам.
    GraphicsConfig* GetGraphicsConfig() const { return graphics_config; }

    // Событие ОС (MAIN-поток): публикует новый размер ОКНА, и только его. Пересоздание таргетов из
    // этого следует, но делает его гейт RenderFunc — окно там лишь один из входов наравне с конфигом.
    // render_w/h не используются (SDL отдаёт их для HiDPI) — оставлены под будущую симметрию.
    void OnWindowResized(Sint32 window_w, Sint32 window_h, Sint32 render_w, Sint32 render_h);
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

    // Размер окна (см. EngineSizeState). Удаление текстур вправе делать ТОЛЬКО render-поток, поэтому
    // пересоздание таргетов исполняет RenderFunc по гейту ниже, а не тот, кто поменял размер.
    EngineSizeState size_state_;

    // Внутреннее разрешение из конфига и размера окна. Общая половина GetWidth/GetHeight — чтобы
    // вывод стоял в одном месте и оба геттера не разъехались.
    void ComputeRenderSize(uint32_t& w, uint32_t& h) const {
        const uint64_t win = size_state_.window_size.load(std::memory_order_relaxed);
        GfxRenderTarget(*graphics_config, EngineSizeState::W(win), EngineSizeState::H(win), w, h);
    }

    // Желаемые размеры (запрос) — против применённых в size_state_ (факт). См. GraphicsConfig.
    GraphicsConfig* graphics_config = nullptr;
    // «Прошлое» гейта размеров: снимок входов, под которые таргеты уже пересозданы.
    // Трогает ТОЛЬКО render-поток.
    TargetSizeInputs applied_inputs_{};

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
