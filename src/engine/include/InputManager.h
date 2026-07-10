#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class EngineContext;

// Идентификаторы интерфейс-команд. По каждому один раз регистрируется функтор
// (см. RegisterCommand), а в очередь летит только id + маленький POD-payload.
// Сами payload-структуры — в InputCommands.h: транспорт их не знает (void*),
// словарь нагрузок нужен только продьюсерам (UI_*) и хендлерам (DefaultCommandSet).
enum class CommandId : uint32_t {
    DeleteEntity,
    HideEntity,   // payload: Entity в младших 32 битах, visible — в бите 32 (см. UI/DefaultCommandSet)
    SetTransform, // payload: SetTransformCmd* на куче (16-float матрица не лезет в указатель),
                  // освобождается функтором после применения (см. DefaultCommandSet)
    SaveScene,    // payload: SceneIOCmd* на куче (имя сцены + путь), освобождает функтор
    LoadScene,    // payload: SceneIOCmd* на куче; грузить сцену можно только в sim-потоке
    SetMaterialTexture, // payload: SetMaterialTextureCmd* на куче (материал+слот+текстура);
                        // sim-поток правит Material::textures[role] + взводит пересборку батчей
    UpsertTexture,      // payload: UpsertTextureCmd* на куче (имя+атлас+путь+old_name). Создать/заменить
                        // текстуру; если old_name != name — это переименование, старую снимаем
    DeleteTexture,      // payload: DeleteTextureCmd* — удалить текстуру (материалы по имени → dummy)
    CreateMaterial,       // payload: CreateMaterialCmd* (имя из UI). Новый материал с sp "sp" + дефолты
    AddMaterialShader,    // payload: MaterialShaderCmd* — добавить sp материалу (+ дефолты НОВЫХ ролей)
    RemoveMaterialShader, // payload: MaterialShaderCmd* — убрать sp у материала
    RenameMaterial,       // payload: RenameMaterialCmd* — ре-кей материала в словаре + пересборка
    UpsertModel,          // payload: UpsertModelCmd* — создать/перезагрузить модель из файла (in-place)
    RebuildShaderPipeline,// payload: RebuildShaderPipelineCmd* — spd правится in-place, тут инвалидация
                          // кэша пайплайна sp + пересборка (пайплайн строится из spd)
    DeleteShader,         // payload: RebuildShaderPipelineCmd* (то же поле shader) — удалить sp:
                          // пайплайн в отложенное удаление + erase sp (шейдеры релизятся по refcount)
    RecreateShader,       // payload: RecreateShaderCmd* — ПЕРЕСОЗДАНИЕ sp (удалить по старому имени,
                          // создать по новому из путей vs/fs) готовым CreateShaderProgram; как Upsert
                          // текстуры/модели. Ссылки материалов по старому имени НЕ чиним → fallback
    SetShaderPass,        // payload: SetShaderPassCmd* — сменить проход sp (associated_render_pass),
                          // инвалидация пайплайна + пересборка (моментально, как spd-тумблеры)
    UpsertVertexShader,   // payload: UpsertVertexShaderCmd*   — создать/пересобрать VSD из формы
    UpsertFragmentShader, // payload: UpsertFragmentShaderCmd* — создать/пересобрать FSD из формы
    UpsertComputeShader,  // payload: UpsertComputeShaderCmd*  — создать/пересобрать CSD из формы
    DeleteVertexShader,   // payload: ShaderDataNameCmd* — удалить VSD из реестра (sp по имени → fallback)
    DeleteFragmentShader, // payload: ShaderDataNameCmd*
    DeleteComputeShader,  // payload: ShaderDataNameCmd*

    COUNT
};

// Транспорт ввода между потоками. Producer — main-поток (HandleEvent),
// consumer — sim-поток (Drain*/Is*/ExecuteCommands). Смысла клавиш не знает:
// дискретные нажатия отдаёт игре, интерфейс-команды исполняет по реестру.
class InputManager {
public:
    // Дискретное "ребро" клавиши — только данные, без функторов.
    struct KeyEvent {
        SDL_Scancode scancode;
        bool         down;
    };

    InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // ---------------- Producer (main-поток) ----------------
    // Единая точка входа: маршрутизирует SDL-событие в снапшоты и очередь.
    void HandleEvent(const SDL_Event& event);

    // ---------------- Consumer (sim-поток) ------------------
    // Выгружает накопленные дискретные события (swap), очищая очередь.
    void DrainKeyEvents(std::vector<KeyEvent>& out);

    // Снимок всех зажатых сейчас клавиш — чтобы перебрать их switch'ем,
    // а не цепочкой IsHeld. Кладёт scancode'ы в переиспользуемый буфер.
    void  SnapshotHeldKeys(std::vector<SDL_Scancode>& out) const;

    // Лок-фри чтение состояния зажатых клавиш / мыши.
    bool  IsHeld(SDL_Scancode sc) const;
    bool  IsMouseButtonDown(uint8_t sdl_button) const;
    float MouseX() const { return mouse_x_.load(std::memory_order_relaxed); }
    float MouseY() const { return mouse_y_.load(std::memory_order_relaxed); }
    // Атомарно забирает и обнуляет накопленный сдвиг колеса.
    float ConsumeWheelDelta() { return wheel_accum_.exchange(0.0f, std::memory_order_relaxed); }

    // ---------------- Интерфейс-команды ---------------------
    // Команда несёт только id и сырой void* на данные — как push_func у шейдера
    // (ShaderData.h / DefaultRenderPassSet.cpp). Под каждую команду пользователь
    // пишет свою структуру (см. InputCommands.h) и сам трактует указатель в функторе:
    // разыменовать структуру, взять указатель на ресурс (модель/текстуру), распаковать
    // число (entity) и т.п. Команда без данных просто получает nullptr — отдельного
    // void-пути нет (ср. DummyDispatchData в DefaultRenderPassSet.cpp).
    using CommandFn = std::function<void(EngineContext*, const void* data)>;

    // Регистрируется один раз: id -> функтор.
    void RegisterCommand(CommandId id, CommandFn fn) {
        registry_[static_cast<size_t>(id)] = std::move(fn);
    }

    // Producer (UI): кладёт id + указатель на данные.
    // ВНИМАНИЕ по времени жизни: данные НЕ копируются. Вызывающий обязан, чтобы
    // указатель оставался валиден до исполнения в sim-потоке — это стабильный
    // ресурс (модель/текстура из менеджера), число, упакованное прямо в указатель,
    // либо объект на куче, который освободит сам функтор.
    void PushCommand(CommandId id, const void* data = nullptr);

    // Consumer (sim): исполняет накопленные команды по реестру.
    void ExecuteCommands(EngineContext* ctx);

private:
    struct InterfaceCommand {
        CommandId   id;
        const void* data;
    };

    // --- снапшоты состояния (пишет main, читает sim) ---
    std::array<std::atomic<bool>, SDL_SCANCODE_COUNT> held_{};
    std::atomic<float>    mouse_x_{ 0.0f };
    std::atomic<float>    mouse_y_{ 0.0f };
    std::atomic<uint32_t> mouse_buttons_{ 0 };   // бит (button-1) выставлен, пока кнопка зажата
    std::atomic<float>    wheel_accum_{ 0.0f };

    // --- очередь дискретных событий клавиатуры ---
    std::mutex            key_mutex_;
    std::vector<KeyEvent> key_events_;

    // --- интерфейс-команды ---
    std::mutex                    cmd_mutex_;
    std::vector<InterfaceCommand> commands_;
    std::vector<InterfaceCommand> commands_scratch_;   // буфер для swap при дренинге
    std::array<CommandFn, static_cast<size_t>(CommandId::COUNT)> registry_{};
};
