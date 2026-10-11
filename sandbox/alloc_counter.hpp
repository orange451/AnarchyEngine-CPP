#pragma once

// Counts heap allocations while armed: every form of the global operator new
// in the sandbox goes through it (alloc_counter.cpp), and Box3D's own
// allocator is counted through PhysicsWorld::count_box3d_allocations.

#include <cstddef>

namespace alloc_counter {

void arm();
void disarm();
// operator new calls while armed, since the last arm().
std::size_t count();

}  // namespace alloc_counter
