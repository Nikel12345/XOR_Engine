#include "PhysicsBufferSet.h"
#include "GpuTaskContext.h"

namespace PhysicsBufferSet {
	void Create(GpuTaskContext* gpu) {
		(void)gpu;
		// TODO: gpu->CreateBufferData("PHYS_TRANSFORM", ...), PHYS_VELOCITY, PHYS_COLLIDERS, PHYS_CONTACTS
		//       + gpu->CreateUpdateInstruction / gpu->CreateReadBackInstruction для них.
	}
}
