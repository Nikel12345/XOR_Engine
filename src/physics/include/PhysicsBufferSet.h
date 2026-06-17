#pragma once

class GpuTaskContext;

// Регистрация буферов физики (PHYS_TRANSFORM / PHYS_VELOCITY / PHYS_COLLIDERS / PHYS_CONTACTS)
// и их update/readback-инструкций — через GPU-фасад. Каркас: пока пусто.
namespace PhysicsBufferSet {
	void Create(GpuTaskContext* gpu);
}
