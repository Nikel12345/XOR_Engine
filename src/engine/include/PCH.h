#pragma once
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
// Часто используемые std-заголовки (по факту инклюдов в headers движка): парсятся один раз в PCH.
#include <functional>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <deque>
#include <map>
#include <set>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <initializer_list>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Utils.h"
// imgui намеренно НЕ в PCH: им пользуются только Engine.cpp и UI_ImGui.cpp (через UI_ImGui.h).
// Так GPU-слой (EngineGpu) не тянет imgui-заголовки.
#include "SDL3_shadercross/SDL_shadercross.h"
