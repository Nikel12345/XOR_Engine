#pragma once
#include <cassert>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <SDL3/sdl.h>
#include <cmath>
#include <mutex>
#include <iostream>

inline Sint32 safe_sint32(float val) {
    assert(std::isfinite(val));
    assert(val >= static_cast<float>(std::numeric_limits<Sint32>::min()));
    assert(val <= static_cast<float>(std::numeric_limits<Sint32>::max()));
    return static_cast<Sint32>(val);
}

inline Uint32 safe_u32(size_t val) {
    assert(val <= std::numeric_limits<Uint32>::max());
    return static_cast<Uint32>(val);
}

inline Uint32 safe_f_u32(float val) {
    assert(std::isfinite(val));
    assert(val >= 0.0f);
    assert(val <= static_cast<float>(std::numeric_limits<Uint32>::max()));
    assert(std::floor(val) == val);
    return static_cast<Uint32>(val);
}

inline int safe_u32t_i(uint32_t val) {
    constexpr uint32_t Imax = static_cast<uint32_t>(std::numeric_limits<int>::max());
    assert(val <= Imax);
    return static_cast<int>(val);
}

inline uint32_t safe_i_u32(int val) {
    assert(val >= 0);
    return static_cast<uint32_t>(val);
}

inline float safe_sint32_f(Sint32 val) {
    return static_cast<float>(val);
}