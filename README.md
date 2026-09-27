# XOR Engine

A C++ 3D/2D game engine built on [SDL3 GPU](https://wiki.libsdl.org/SDL3/CategoryGPU). Strict ECS architecture. Cross-platform via SDL3: Windows, Linux, macOS.

> Early stage. Focus is on the technical foundation, not user-facing tooling.

---

## Core Philosophy

XOR Engine is designed around two ideas: **raw performance** and **low-level access without friction**.

You can use it as a conventional engine — compose a scene, attach components, run systems. Or you can go deeper: replace the entire render pipeline, write custom passes, manage your own GPU buffers. The abstraction layer gets out of your way when you need it to.

There is no scripting layer. Game logic is written in plain C++ using engine systems.

---

## Features

- **ECS** — all game objects, logic, and state live in entities, components, and systems
- **SDL3 GPU backend** — cross-platform Vulkan/Metal/D3D12 via SDL3's GPU API
- **Custom render pipeline** — swap out or extend the default renderer; write your own passes and buffers from scratch
- **GPU-driven rendering** — custom GPU-driven passes supported out of the box
- **Minimal memory overhead** — near-zero abstraction cost on memory operations; direct control where it matters
- **Library-first** — integrate into your project by including the engine as a library; your game code stays in your own `game` translation unit

---

## Planned

- [ ] Scene editor
- [ ] Physics

---

## Getting Started

**Requirements:** CMake 3.x+, a C++20-capable compiler. SDL3 is fetched automatically via CMake.

### Build

```bash
git clone https://github.com/Nikel12345/XOR_Engine.git
cd XOR_Engine
cmake -B build
cmake --build build
```

This builds the engine libraries (`Engine`, `Physics`). A game is a separate CMake project that
adds the engine as a subdirectory:

```cmake
set(XOR_ENGINE_DIR "path/to/XOR_Engine" CACHE PATH "")
add_subdirectory("${XOR_ENGINE_DIR}" xor_engine)

add_executable(MyGame main.cpp)
target_link_libraries(MyGame PRIVATE Engine Physics)
copy_runtime_dlls(MyGame)
```
---

## Documentation

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — build targets, roles inside the engine, extension
seams, threading model, and where to read next. Deeper notes on individual subsystems live in
[`docs/`](docs). Written in Russian.

---

## License

No license defined yet. All rights reserved.
