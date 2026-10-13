#pragma once

#include "Matrix4.hpp"
#include "ShadowMath.hpp"
#include "ShadowPlanner.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
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
    // The distance band it draws in (MeshDraw::lodNear, lodFar), from the
    // camera to model's origin. Outside it, it is neither seen nor casts.
    float lodNear = 0.f;
    float lodFar = std::numeric_limits<float>::infinity();
    // A billboard (MeshDraw::billboard): its instance carries model as it is,
    // which the vertex shader turns to face the camera, and no normal matrix.
    bool billboard = false;
    // It may cast a shadow (MeshDraw::castsShadow).
    bool castsShadow = false;
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
    // One per DrawItem, beside its sphere: a banded one's band, measured
    // from its model's origin, ends squared. Read only with kTraitBanded.
    struct Band {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        float nearSquared = 0.f;
        float farSquared = 0.f;
    };
    std::vector<Band> bands;
    // One per DrawItem, set by CullDraws: 1 for a banded one the camera is
    // outside the band of, which is neither visible nor casts a shadow.
    std::vector<std::uint8_t> outOfBand;
    // Moves on whenever an outOfBand flag changes, or their count.
    std::uint64_t outOfBandRevision = 0;
    // The drawable items that may cast and are in their band, in item order:
    // the only ones a shadow can take in, for the shadow planner.
    std::vector<int> casters;
    // In DrawItem order.
    std::vector<VisibleDraw> opaque;
    std::vector<VisibleDraw> transparent;
    // Drawable, but outside the view.
    int culled = 0;

    // Each block's lists while FindVisible runs blocks apart, joined in block order.
    struct Block {
        std::vector<VisibleDraw> opaque;
        std::vector<VisibleDraw> transparent;
        std::vector<int> casters;
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
// It draws only in a distance band (DrawItem::lodNear, lodFar).
constexpr std::uint16_t kTraitBanded = 8;
// It may cast a shadow (DrawItem::castsShadow).
constexpr std::uint16_t kTraitCasts = 16;

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
