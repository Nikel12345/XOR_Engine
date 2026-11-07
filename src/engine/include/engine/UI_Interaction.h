#pragma once
#include <SDL3/SDL_scancode.h>
#include <cstdint>
#include <functional>
#include <string>
#include "Aliases.h"

using UIClickFunc = std::function<void(const UIVarKey& instance)>;

struct UIClick {
    std::string id;
    UIVarKey    instance;
};

enum class UIInputKind : uint8_t { Text, Key };

struct UIInputEvent {
    UIInputKind  kind = UIInputKind::Key;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    std::string  text;
};
