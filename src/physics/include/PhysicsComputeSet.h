#pragma once

class GpuTaskContext;

// Регистрация compute-программ физики (integrate / broad-phase / narrow-phase).
// Берёт узкий GPU-фасад GpuTaskContext — без EngineContext и рендера.
namespace PhysicsComputeSet {
	void Create(GpuTaskContext* gpu);
}
