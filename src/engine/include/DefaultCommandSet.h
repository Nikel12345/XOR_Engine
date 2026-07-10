#pragma once

class InputManager;

// Регистрация билтин UI-команд движка. Паттерн зеркалит DefaultUpdateSet/DefaultRenderPassSet
// (свободные функции-сеттеры вместо метода Engine), но принимает InputManager& — регистрируем
// именно в его реестре. Лямбды stateless: всё берут из EngineContext* + void*-payload, полей
// Engine не касаются, поэтому живут отдельным TU и Engine.h не тянут.
namespace DefaultCommandSet
{
	void SetEntityCommands(InputManager& im);    // Delete/Hide/SetTransform/Create (из staging-формы)
	void SetSceneCommands(InputManager& im);     // Save/Load сцены-папки
	void SetMaterialCommands(InputManager& im);  // texture-слот/create/add-remove-sp/rename
	void SetTextureCommands(InputManager& im);   // upsert/delete текстуры
	void SetModelCommands(InputManager& im);     // upsert модели из файла
	void SetShaderCommands(InputManager& im);    // rebuild/delete/recreate/setpass sp + upsert/delete VS/FS/CS

	// Разом все группы (зовётся из Engine::InitUICommands).
	inline void SetAll(InputManager& im)
	{
		SetEntityCommands(im);
		SetSceneCommands(im);
		SetMaterialCommands(im);
		SetTextureCommands(im);
		SetModelCommands(im);
		SetShaderCommands(im);
	}
}
