#pragma once
#include <SDL3/SDL.h>
#include "Engine.h"
#include "InputManager.h"

struct ModelData;   // члены-указатели: полные типы тянет Game.cpp
struct Material;

enum class GameState {
	MAIN_MENU,
	GAMEPLAY,
	EDITOR,
	SETTINGS
};

struct MainMenuResources {
};

class Game {
public:
	Game(Engine* engine);
	SDL_AppResult MainInit();
	SDL_AppResult MainIterate();
	void SDL_AppQuit();

	bool lmb_down = false;
	bool rmb_down = false;
private:
	Engine* engine = nullptr;
	
	TextureManager* textureManager;
	ModelManager* modelManager;
	ObjectManager* objectManager;
	CameraManager* cameraManager;

	ThreadController* threadController;
	InputManager* input;


	EngineContext* ctx;
	std::vector<InputManager::KeyEvent> key_events_scratch;
	std::vector<SDL_Scancode> held_keys_scratch;
	void ChangeState(GameState newState);
	void MainMenu_Init();
	void MainMenu_Iterate();
	void MainMenu_Update();
	void MainMenu_Event(SDL_Event* event);
	void MainMenu_Quit();

	// Гравитация к центрам-сущностям (GravityComponent) + интеграция позиций скоростями.
	// Плюс отскок: коснувшись центра (координатная рамка), сущность получает vy = ±10, vx/vz = 0.
	void SimulateGravity();
	// Наведение курсора на UI: пересечение курсора с ректом узла → второй albedo, иначе первый.
	void UpdateUIHover();

	// Снимок центров тяжести на тик: сначала собираем их (их единицы), потом один проход по
	// притягиваемым. Член, а не локальная переменная, — чтобы не аллоцировать каждый тик.
	// hx/hy/hz — полу-размеры источника В МИРЕ (локальный AABB модели x масштаб трансформа):
	// «коснуться» значит войти в тот куб, который видно на экране, а не в рамку фиксированного
	// размера вокруг его центра.
	struct GravitySource { float x, y, z, gm; float hx, hy, hz; };
	std::vector<GravitySource> gravity_sources;

	// Глубина сущности в рамках источников за тик (<= 0 — внутри). Переносное хранилище
	// между подпроходами SimulateGravity: горячий цикл пишет её ЗНАЧЕНИЕМ, потому что
	// условная запись флага ломает векторизацию. Член, а не локаль, — чтобы не
	// перевыделять 800k float каждый тик.
	std::vector<float> touch_depth;

	float width;
	float height;
	float mouse_x = 0;
	float mouse_y = 0;
	GameState game_state;
	MainMenuResources main_menu_resources;

	void CreateDebugColliders(); 
	ModelData* debug_box_model = nullptr;    // куб [-1..1]
	ModelData* debug_sphere_model = nullptr; // сфера r=1
	Material*  debug_collider_material = nullptr;
};