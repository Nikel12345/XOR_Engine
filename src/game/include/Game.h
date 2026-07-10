#pragma once
#include <SDL3/SDL.h>
#include "Engine.h"
#include "InputManager.h"   // InputManager::KeyEvent в скретч-буферах ниже

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

	// Гравитация центрального объекта (0,0) в плоскости XZ + интеграция позиций скоростями.
	void SimulateGravity();

	float width;
	float height;
	float mouse_x = 0;
	float mouse_y = 0;
	GameState game_state;
	MainMenuResources main_menu_resources;

	// --- Отладочная визуализация коллайдеров ---
	void CreateDebugColliders(); 
	ModelData* debug_box_model = nullptr;    // куб [-1..1]
	ModelData* debug_sphere_model = nullptr; // сфера r=1
	Material*  debug_collider_material = nullptr;
};