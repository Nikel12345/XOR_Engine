#pragma once

class GpuContext;

// Регистрация буферов физики (PHYS_TRANSFORM / PHYS_VELOCITY / PHYS_COLLIDERS / PHYS_CONTACTS)
// и их update/readback-инструкций — через GPU-фасад. Каркас: пока пусто.
namespace PhysicsBufferSet {
	void Create(GpuContext* gpu);
}
