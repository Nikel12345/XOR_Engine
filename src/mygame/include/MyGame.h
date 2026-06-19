#pragma once
#include <SDL3/SDL.h>
#include "Engine.h"

// Заготовка второй игры. Своя оболочка над общим Engine: собственный набор сцен,
// материалов и проходов живёт здесь и не пересекается с Game, потому что MyGame
// собирается в отдельный exe со СВОИМ инстансом Engine (свой PassManager и т.д.).
//
// Контракт намеренно минимальный — один Init / один Iterate / один Event.
// Состояния/режимы (меню, геймплей) добавляй здесь по мере надобности.
class MyGame {
public:
    MyGame(Engine* engine);

    SDL_AppResult MainInit();                  // создать сцену, ресурсы, сущности
    SDL_AppResult MainIterate();               // один sim-тик (зовётся из ThreadController)
    SDL_AppResult SDL_AppEvent(SDL_Event* event);

private:
    Engine* engine = nullptr;

    TextureManager* textureManager = nullptr;
    ModelManager*   modelManager   = nullptr;
    ObjectManager*  objectManager  = nullptr;
    CameraManager*  cameraManager  = nullptr;

    ThreadController* threadController = nullptr;
    InputManager*     input           = nullptr;

    EngineContext* ctx = nullptr;              // узкий фасад движка для создания ресурсов/сущностей

    float width  = 0.0f;
    float height = 0.0f;
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;

    // Скретч-буферы под дренаж ввода (переиспользуются между кадрами, без аллокаций).
    std::vector<InputManager::KeyEvent> key_events_scratch;
    std::vector<SDL_Scancode>           held_keys_scratch;
};
