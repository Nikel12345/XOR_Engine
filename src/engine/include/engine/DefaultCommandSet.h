#pragma once

class InputManager;

namespace DefaultCommandSet
{
	void SetEntityCommands(InputManager& im);
	void SetSceneCommands(InputManager& im);
	void SetMaterialCommands(InputManager& im);
	void SetTextureCommands(InputManager& im);
	void SetModelCommands(InputManager& im);
	void SetShaderCommands(InputManager& im);

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
