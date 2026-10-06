#pragma once

// A Custom PhysicsObject's Mesh as convex pieces, for a body that moves: Box3D
// gives a triangle mesh contacts only on a static body. V-HACD makes them.

#include "Vector3.hpp"
#include "amesh.hpp"

#include <cstdint>
#include <vector>

namespace engine_core {

class Mesh;

// Which settings made a set of pieces. Bump it whenever a setting in
// ConvexDecomposition.cpp changes, so pieces stored with the old ones are made again.
inline constexpr std::uint32_t kRecipe = 1;

// The pieces of a mesh, in its own space. Empty when there is nothing to
// decompose or V-HACD finds no piece. Any thread; seconds on a large mesh.
std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles);

// Pieces kept in memory, keyed by the geometry they were made from and
// kRecipe: the last 64 meshes. Any thread.
void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces);

// The Mesh's pieces without decomposing: its file's, else the cache's for
// points and triangles (the Mesh's, as vertex_positions gives them). True when
// they are known, even known to be none.
bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out);

// known_pieces, else decompose now and remember the result.
std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles);

// For tests: how many times decompose has run, and forgetting every cached piece.
std::uint64_t decompose_count();
void clear_piece_cache();

}  // namespace engine_core
