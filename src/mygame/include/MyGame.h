#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include "Engine.h"
#include "InputManager.h"   // InputManager::KeyEvent в скретч-буферах

// ============================================================================
//  MyGame — фрактальная демка. Сцена выбирает фрактал (scene_fractal — 3D-губка
//  Менгера рэймарчем, scene_mandelbrot — Мандельброт перетурбацией): рендер-
//  ресурсы фона — в манифестах saved_scene_*, энтити — в их scene.json; в коде
//  только кадр фрактала (буфер в MainInit + апдейтер FractalUpdateSet, под if'ом
//  с именем сцены) и ввод: ЛКМ — вращение, стрелки/Space/LShift — полёт,
//  колесо — скорость, I/K — ручной сдвиг окна масштаба.
// ============================================================================
class MyGame {
public:
	MyGame(Engine* engine);

	SDL_AppResult MainInit();
	SDL_AppResult MainIterate();

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

	std::vector<InputManager::KeyEvent> key_events_scratch;
	std::vector<SDL_Scancode>           held_keys_scratch;
};
