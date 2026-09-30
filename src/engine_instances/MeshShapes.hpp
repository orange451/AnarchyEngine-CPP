#pragma once

#include "Vector3.hpp"
#include "amesh.hpp"

namespace engine_core {

// The shapes a Mesh's Add methods build, each appended to data in the engine's
// space: Y up, CCW front faces facing out, normals set, white, UVs across
// each face or around each side. Sizes are in world units and must be finite
// and above 0; segment counts are clamped to kMinShapeSegments..kMaxShapeSegments.
// Every shape but the teapot is centered on at.
inline constexpr int kMinShapeSegments = 3;
inline constexpr int kMaxShapeSegments = 256;

void add_box(anarchy::amesh::Data& data, Vec3 size, Vec3 at);
// A UV sphere: segments around, segments / 2 from pole to pole.
void add_sphere(anarchy::amesh::Data& data, float radius, int segments, Vec3 at);
// Along Y, height tall. capped closes both ends.
void add_cylinder(anarchy::amesh::Data& data, float radius, float height, int segments, bool capped, Vec3 at);
// Along Y: the base at the bottom, the point at the top. capped closes the base.
void add_cone(anarchy::amesh::Data& data, float radius, float height, int segments, bool capped, Vec3 at);
// Flat on XZ, facing +Y.
void add_plane(anarchy::amesh::Data& data, float width, float depth, Vec3 at);
// A teapot size tall, standing on at, its spout toward +X. Built from turned
// outlines and swept tubes in the proportions of Newell's Utah teapot, not
// from its Bezier patches.
void add_teapot(anarchy::amesh::Data& data, float size, Vec3 at);

}  // namespace engine_core
