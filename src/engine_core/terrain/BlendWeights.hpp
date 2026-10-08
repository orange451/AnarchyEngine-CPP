#pragma once

// Task 2 of the terrain textures plan: up to 4 blended material Ids per
// terrain render vertex, so the Task 6 shader can blend materials smoothly
// without a geometry shader. See docs/superpowers/specs/2026-10-07-terrain-
// textures-design.md, "Blend weights in the mesh" and implementation note 1.

#include "amesh.hpp"

#include <array>
#include <cstdint>

namespace engine_core::terrain {

// Up to 4 material Ids with weights summing to 1 (an unused slot: Id 0,
// weight 0 -- but a used slot may also legitimately carry Id 0, the default
// material, with a nonzero weight).
struct BlendIds {
    std::array<std::uint8_t, 4> ids{0, 0, 0, 0};
    std::array<float, 4> weights{0.f, 0.f, 0.f, 0.f};
};

// From one Surface Nets cell's 8 corners (distance[8]/id[8], in
// SurfaceNets.cpp's kCorner order -- though the rule below does not depend
// on that order): every corner within voxel_size of the surface
// (|distance| <= voxel_size) counts one vote for its Id; votes are
// normalized into weights, the top 4 Ids kept (ties broken by lower Id) and
// renormalized to sum to 1, sorted by descending weight. When no corner
// qualifies (every corner deep in solid or air), the lowest-distance
// corner's Id alone, at weight 1 -- the same rule Surface Nets and
// VoxelSampler used for a vertex's single Id before blending.
BlendIds blend_weights(const float distance[8], const std::uint8_t id[8], float voxel_size);

// Splits every triangle of render whose three vertices do not carry
// identical sets of Ids with nonzero weight: it gets three new vertices
// (copies of its corners, same position/normal/uv/etc.) carrying the
// triangle's merged set -- the top 4 Ids by summed weight over its three
// corners (ties by lower Id) -- with each corner's own weights mapped onto
// that set and renormalized to sum to 1. A triangle whose three corners
// already agree keeps its shared vertices untouched. Vertices no triangle
// indexes any more afterward are compacted away (and vertex indices
// renumbered accordingly). Render mesh only: collision positions/triangles
// and physics Ids are a separate representation and are never touched here.
// Any thread.
void split_border_triangles(anarchy::amesh::Data& render);

}  // namespace engine_core::terrain
