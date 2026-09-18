#pragma once

class GpuContext;

// ЗАГОТОВКА: тело пустое, Create не зовёт ни одна игра. Это шов, через который физика
// получит GPU, не линкуя Engine, — а не мёртвый код.
namespace PhysicsBufferSet {
	void Create(GpuContext* gpu);
}
