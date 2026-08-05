#pragma once
#include <SDL3/SDL.h>
#include <vector>
#include <glm/glm.hpp>
#include "Engine.h"
#include "InputManager.h"

struct Material;    // ресурсы спавна якорённых кубов — только указатели
struct ModelData;

//  MyGame — фрактальная демка. Сцена выбирает фрактал (scene_fractal — 3D-губка
//  Менгера рэймарчем, scene_mandelbrot — Мандельброт перетурбацией): рендер-
//  ресурсы фона — в манифестах saved_scene_*, энтити — в их scene.json; в коде
//  только кадр фрактала (буфер в MainInit + апдейтер FractalUpdateSet, под if'ом
//  с именем сцены) и ввод: ЛКМ — вращение, стрелки/Space/LShift — полёт,
//  колесо — скорость, I/K — ручной сдвиг окна масштаба.
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

	// Сцена губки активна (MainIterate гоняет MengerTick + матрицы якорённых объектов).
	bool fractal_scene = false;
	// Ресурсы спавна якорённых кубов (этап 4): захвачены в MainInit — клавиша N создаёт
	// энтити с FractalAnchorComponent из них (материал/модель общие для всех кубов).
	Material*  anchor_material = nullptr;
	ModelData* cube_model      = nullptr;

	// Переноска (этап 8b, HUD-вариант): G забирает выбор у UI (гизмо гаснет само), и куб
	// ВЫКЛЮЧАЕТСЯ из мира — рисуется в фиксированной части экрана всегда одинаково (HUD-поза
	// в ForEach-контуре MainIterate); свои ориентация и пропорции (model_scale) сохраняются.
	// Повторное G кладёт его в мир заново спавн-позой (1.5σ перед камерой, σ/2) — личная
	// логика масштаба доводит размер под окружение сама. Несём один объект за раз.
	bool     carrying = false;
	uint32_t carried  = 0;

	std::vector<InputManager::KeyEvent> key_events_scratch;
	std::vector<SDL_Scancode>           held_keys_scratch;
};
