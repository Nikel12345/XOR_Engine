#include "PhysicsComputeSet.h"
#include "GpuTaskContext.h"

namespace PhysicsComputeSet {
	void Create(GpuTaskContext* gpu) {
		(void)gpu;
		// TODO: auto cs = gpu->CreateComputeShader("physics/integrate.comp.spv");
		//       gpu.CreateComputeShaderProgram("phys_integrate", cs,
		//           { "PHYS_TRANSFORM", "PHYS_VELOCITY" }, {}, {}, {}, {}, "PHYS_PREPASS");
	}
}
