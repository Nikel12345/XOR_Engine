#include <chrono>
#include <iostream>
#include <string>
#include <algorithm>
#include <cmath>
#include <random>
#include <array>
#include <unordered_map>
#include <vector>
#include <memory>
#include <unordered_set>
#include <cstdint>
#include <span>
#include <variant>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL.h>
// SDL3_image намеренно НЕ в PCH: единственный потребитель — TextureLoader,
// он подключает его сам. Так движок (и будущие либы вроде физики) не тянут image.
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Utils.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "SDL3_shadercross/SDL_shadercross.h"
