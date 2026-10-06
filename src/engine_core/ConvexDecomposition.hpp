#pragma once

// A Custom PhysicsObject's Mesh as convex pieces, for a body that moves: Box3D
// gives a triangle mesh contacts only on a static body. V-HACD makes them.

#include "Vector3.hpp"
#include "amesh.hpp"

#include <cstdint>
#include <vector>

namespace engine_core {

// Which settings made a set of pieces. Bump it whenever a setting in
// ConvexDecomposition.cpp changes, so pieces stored with the old ones are made again.
inline constexpr std::uint32_t kRecipe = 1;

// The pieces of a mesh, in its own space. Empty when there is nothing to
// decompose or V-HACD finds no piece. Any thread; seconds on a large mesh.
std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles);

}  // namespace engine_core
