#pragma once

#include "Matrix4.hpp"
#include "ShadowMath.hpp"
#include "ShadowPlanner.hpp"

#include <cstdint>
#include <vector>

namespace runner {

// What visibility and batching read from one MeshDraw, with no GL: the
// Renderer fills one per draw each frame, pointing into that draw.
struct DrawItem {
    // World space, column-major.
    const engine_core::Matrix4* model = nullptr;
    // The mesh's local box.
    const float* boundsMin = nullptr;
    const float* boundsMax = nullptr;
    // The GameObject's Color, RGB as its Color3 holds it. Null is white.
    const float* tint = nullptr;
    // 0 is opaque; above 0 is drawn in the see-through pass.
    float transparency = 0.f;
    // Which Prefab Model it draws. Draws with the same nonzero slot share a
    // mesh and every Material value. 0 never batches.
    std::uint32_t slot = 0;
    // An uploaded mesh, and transparency below 1. Anything else draws nothing.
    bool drawable = false;
};

// A drawable mesh the camera may see.
struct VisibleDraw {
    // Into the frame's DrawItems, and so its MeshDraws.
    int index = 0;
    // Its sphere's radius as projected, in pixels; infinite with the camera inside it.
    float screenRadius = 0.f;
    // Which LOD it draws. 0 until LOD selection fills it from screenRadius.
    std::uint8_t lod = 0;
};

// One frame's visibility, reused frame to frame so it allocates only while it grows.
struct VisibilityResult {
    // One per DrawItem, world space, for the shadow pass too. Zero for one that is not drawable.
    std::vector<Sphere> spheres;
    // In DrawItem order.
    std::vector<VisibleDraw> opaque;
    std::vector<VisibleDraw> transparent;
    // Drawable, but outside the view.
    int culled = 0;
};

// Each drawable item's world sphere and, when cull is set, whether it is in
// camera's view. With cull false every drawable item is visible. Conservative:
// an item just past a corner of the view may count as visible; one in view never
// counts as culled.
void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out);

// sphere's radius in pixels as camera sees it: radius * paneHeight /
// (2 * distance * tan(fovY / 2)). Infinite when the camera is inside it.
float ScreenRadius(const Sphere& sphere, const CameraView& camera);

}  // namespace runner
