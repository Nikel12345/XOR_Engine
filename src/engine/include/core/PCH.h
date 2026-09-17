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
// imgui сюда НЕЛЬЗЯ: этот PCH форс-инклюдится в EngineCore/Ecs/Gpu, а ImGui линкуется к Engine
// как PRIVATE — ниже его include-путей нет, и общий PCH с imgui там просто не соберётся.
// Верхнему слою imgui даёт PCH_Engine.h.
