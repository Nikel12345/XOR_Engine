#pragma once

class GpuContext;

// Регистрация compute-программ физики (integrate / broad-phase / narrow-phase).
// Берёт узкий GPU-фасад GpuContext — без EngineContext и рендера.
namespace PhysicsComputeSet {
	void Create(GpuContext* gpu);
}
