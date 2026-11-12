#pragma once
#include <SDL3/SDL_scancode.h>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include "Aliases.h"

using UIClickFunc = std::function<void(const UIVarKey& instance)>;

struct UIClick {
    std::string id;
    UIVarKey    instance;
};

struct UITextInput {
    std::string text;
};

struct UIKeyInput {
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
};

struct UIWheelInput {
    float x = 0.0f, y = 0.0f;
    float window_x = 0.0f, window_y = 0.0f;
};

using UIInputEvent = std::variant<UITextInput, UIKeyInput, UIWheelInput>;
