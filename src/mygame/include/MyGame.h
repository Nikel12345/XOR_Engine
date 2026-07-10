#pragma once
#include <SDL3/SDL.h>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <typeindex>
#include "Engine.h"
#include "InputManager.h"    // InputManager::KeyEvent в скретч-буферах
#include "BaseComponents.h"  // Entity (раньше транзитивно через Engine.h)

// ============================================================================
//  Каркас "игры с режимами" поверх общего Engine. Всё намеренно в одном файле:
//  это слой ПРИЛОЖЕНИЯ (не движка), он gameplay-специфичен и принадлежит игре.
//  Если устаканится и понадобится в нескольких играх — выносится в отдельный
//  framework-заголовок, но НЕ внутрь Engine (тот остаётся рендер/ресурсным ядром).
// ============================================================================

// ---------------------------------------------------------------------------
//  EventBus — общий (на всю игру) транспорт событий. Знает 0 конкретных типов:
//  типы и реакции навешивают режимы. Тип-эрейзед очередь + диспетчер раз за тик.
//  Почему очередь, а не прямой вызов обработчика: реакция может спавнить/удалять
//  сущности, а это нельзя делать внутри ForEach — отложенный Dispatch ставит все
//  мутации в безопасную точку (тот же принцип, что "собрать мёртвых → удалить").
// ---------------------------------------------------------------------------
class EventBus {
public:
    // Подписка на тип события E. Режимы зовут на Enter.
    template<class E>
    void Subscribe(std::function<void(const E&)> fn) {
        handlers_[std::type_index(typeid(E))].push_back(
            [fn = std::move(fn)](const void* p) { fn(*static_cast<const E*>(p)); });
    }

    // Эмит: кто угодно кладёт событие в очередь кадра (данные копируются).
    template<class E>
    void Emit(const E& e) {
        queue_.push_back({ std::type_index(typeid(E)), std::make_shared<E>(e) });
    }

    // Разбор очереди — один раз за тик (зовёт хост после Update режима).
    // swap в scratch: обработчик может эмитить новое событие, не ломая обход.
    void Dispatch() {
        scratch_.swap(queue_);
        for (Msg& m : scratch_) {
            auto it = handlers_.find(m.type);
            if (it == handlers_.end()) continue;
            for (auto& h : it->second) h(m.payload.get());
        }
        scratch_.clear();
    }

    // Снять все подписки. Хост зовёт при смене режима: следующий режим
    // переподпишет своё в Enter. Когда появятся ДОЛГОЖИВУЩИЕ слушатели
    // (звук/ачивки), замени на токен-отписку по владельцу.
    void ClearHandlers() { handlers_.clear(); }

private:
    struct Msg { std::type_index type; std::shared_ptr<void> payload; };
    std::unordered_map<std::type_index, std::vector<std::function<void(const void*)>>> handlers_;
    std::vector<Msg> queue_, scratch_;
};

// ---------------------------------------------------------------------------
//  Конкретные события игры — только данные. Движок про них не знает.
// ---------------------------------------------------------------------------
struct ReachedZone  { Entity who;   int zone_id; };
struct EnemyKilled  { Entity enemy; };
struct TimerExpired { int timer_id; };

// ---------------------------------------------------------------------------
//  Сервисы, которые хост отдаёт каждому режиму: примитивы движка + общая шина.
//  Режим — тонкий клей поверх них, кишек Engine ему не нужно.
// ---------------------------------------------------------------------------
struct ModeContext {
    Engine*         engine        = nullptr;
    EngineContext*  ctx           = nullptr;
    ObjectManager*  objectManager = nullptr;
    CameraManager*  cameraManager = nullptr;
    InputManager*   input         = nullptr;
    EventBus*       bus           = nullptr;
    float           width  = 0.0f;
    float           height = 0.0f;
};

// ---------------------------------------------------------------------------
//  IGameMode — режим = код одной сцены. 1 режим ↔ 1 ECS-сцена.
//  Ввода-метода нет специально: игровой ввод тянется из InputManager в Update,
//  а события окна живут в main.cpp (см. развязку SDL_AppEvent).
// ---------------------------------------------------------------------------
class IGameMode {
public:
    virtual ~IGameMode() = default;
    virtual void Enter()          = 0;  // CreateScene + SetActiveScene, ресурсы, СВОИ подписки/триггеры
    virtual void Update(float dt) = 0;  // производители событий (зоны/таймеры) → Emit
    virtual void Exit() {}              // деактивировать сцену / снять триггеры
};

// ----- Меню: своя сцена, своя (минимальная) логика, своих событий нет --------
class MainMenuMode : public IGameMode {
public:
    explicit MainMenuMode(const ModeContext& c) : s_(c) {}
    void Enter() override;
    void Update(float dt) override;
private:
    ModeContext s_;
};

// ----- Геймплей: своя сцена, свои триггеры/таймеры и свои реакции ------------
class GameplayMode : public IGameMode {
public:
    explicit GameplayMode(const ModeContext& c) : s_(c) {}
    void Enter() override;
    void Update(float dt) override;
    void Exit() override;
private:
    struct Timer { int id; float remaining; bool done = false; };

    ModeContext         s_;
    std::vector<Timer>  timers_;
    std::vector<Entity> dead_scratch_;
};

// ---------------------------------------------------------------------------
//  MyGame — хост приложения: владеет общей шиной и текущим режимом, прокидывает
//  общий ввод/камеру и форвардит тик активному режиму. Машины состояний-enum
//  больше нет — режим полиморфен, добавление сцены = новый класс + ChangeMode.
// ---------------------------------------------------------------------------
class MyGame {
public:
    MyGame(Engine* engine);

    SDL_AppResult MainInit();
    SDL_AppResult MainIterate();

    // Сменить режим: Exit старого → сброс подписок → новый владелец → Enter.
    void ChangeMode(std::unique_ptr<IGameMode> next);

private:
    Engine* engine = nullptr;

    TextureManager* textureManager = nullptr;
    ModelManager*   modelManager   = nullptr;
    ObjectManager*  objectManager  = nullptr;
    CameraManager*  cameraManager  = nullptr;

    ThreadController* threadController = nullptr;
    InputManager*     input           = nullptr;

    EngineContext* ctx = nullptr;

    float width  = 0.0f;
    float height = 0.0f;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;

    EventBus                    bus;          // общая шина на всю игру
    ModeContext                 mode_ctx;     // сервисы, раздаваемые режимам
    std::unique_ptr<IGameMode>  current;      // активный режим

    std::vector<InputManager::KeyEvent> key_events_scratch;
    std::vector<SDL_Scancode>           held_keys_scratch;
};
