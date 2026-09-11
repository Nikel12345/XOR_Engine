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

The repository already includes a test scene in the `game` class — it's the author's sandbox for development. You can use it as a reference or modify it directly to build your own scene.

**Requirements:** CMake 3.x+, a C++20-capable compiler. SDL3 is fetched automatically via CMake.

### Build

#### C++ step
```bash
git clone https://github.com/Nikel12345/XOR_Engine.git
cd XOR_Engine
cmake -B build
cmake --build build
```
#### Python step
```bash
cd scripts/model_loader
python -m venv venv
pip install -r requirements.txt
```
---

## Documentation

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — build targets, roles inside the engine, extension
seams, threading model, and where to read next. Deeper notes on individual subsystems live in
[`docs/render-pipeline/`](docs/render-pipeline). Written in Russian.

---

## License

No license defined yet. All rights reserved.
