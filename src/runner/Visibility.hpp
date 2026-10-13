#pragma once

#include "Matrix4.hpp"
#include "ShadowMath.hpp"
#include "ShadowPlanner.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace runner {

// Draws [first, first + count).
struct DrawRange {
    std::size_t first = 0;
    std::size_t count = 0;
};

// ranges sorted and joined: ascending, no two touching or overlapping, none empty.
void NormalizeRanges(std::vector<DrawRange>& ranges);

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
    // A terrain chunk (MeshDraw::terrainLook): never batched, and sorted after
    // every other opaque draw.
    bool terrain = false;
    // An uploaded mesh, and transparency below 1. Anything else draws nothing.
    bool drawable = false;
    // Its skinning matrices' first texel in the frame's bone texture, or -1
    // for none. The renderer sets it; batching copies it into each instance.
    int boneBase = -1;
    // Shadows only (MeshDraw::shadowOnly): its sphere is found, but it is never visible.
    bool shadowOnly = false;
    // Which of its mesh's LODs it draws (MeshDraw::lod), copied into its VisibleDraw.
    std::uint8_t lod = 0;
};

// A drawable mesh the camera may see.
struct VisibleDraw {
    // Into the frame's DrawItems, and so its MeshDraws.
    int index = 0;
    // Its sphere's radius as projected, in pixels; infinite with the camera inside it. Read by tests and kept for a future LOD pick.
    float screenRadius = 0.f;
    // Which LOD it draws: its DrawItem's.
    std::uint8_t lod = 0;
};

// One frame's visibility, reused frame to frame so it allocates only while it grows.
struct VisibilityResult {
    // One per DrawItem, world space, for the shadow pass too. Zero for one that is not drawable.
    std::vector<Sphere> spheres;
    // One per DrawItem, beside its sphere: what culling reads of it, the
    // kTrait bits and its LOD in the high byte.
    std::vector<std::uint16_t> traits;
    // In DrawItem order.
    std::vector<VisibleDraw> opaque;
    std::vector<VisibleDraw> transparent;
    // Drawable, but outside the view.
    int culled = 0;

    // Each block's lists while FindVisible runs blocks apart, joined in block order.
    struct Block {
        std::vector<VisibleDraw> opaque;
        std::vector<VisibleDraw> transparent;
        int culled = 0;
    };
    std::vector<Block> blocks;
};

// Items FindVisible gives each worker thread at a time.
constexpr std::size_t kVisibilityGrain = 8192;

// VisibilityResult::traits bits.
constexpr std::uint16_t kTraitDrawable = 1;
constexpr std::uint16_t kTraitShadowOnly = 2;
constexpr std::uint16_t kTraitTransparent = 4;

// The spheres and traits of the items in ranges (normalized), or of every
// item when all, into out's, which are sized count; the others are kept as
// they were. Blocks on worker threads.
void UpdateDrawBounds(const DrawItem* items, int count, bool all, const std::vector<DrawRange>& ranges,
                      VisibilityResult& out, std::size_t grain = kVisibilityGrain);

// Which items camera sees, from out's spheres and traits alone, into out's
// lists, as FindVisible finds them.
void CullDraws(int count, const CameraView& camera, bool cull, VisibilityResult& out,
               std::size_t grain = kVisibilityGrain);

// Each drawable item's world sphere and, when cull is set, whether it is in
// camera's view. With cull false every drawable item is visible but a
// shadowOnly one, which is never visible. Conservative: an item just past a
// corner of the view may count as visible; one in view never counts as culled.
// Blocks of grain items run on worker threads; the lists come out in item
// order whatever the grain.
void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out,
                 std::size_t grain = kVisibilityGrain);

// sphere's radius in pixels as camera sees it: radius * paneHeight /
// (2 * distance * tan(fovY / 2)). Infinite when the camera is inside it.
float ScreenRadius(const Sphere& sphere, const CameraView& camera);

}  // namespace runner
