#pragma once

#include "Matrix4.hpp"
#include "ShadowMath.hpp"
#include "ShadowPlanner.hpp"

#include <algorithm>
#include <cmath>
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
    // camera to model's origin. Once it has faded out of it (StepLodFade),
    // it is neither seen nor casts.
    float lodNear = 0.f;
    float lodFar = std::numeric_limits<float>::infinity();
    // A billboard (MeshDraw::billboard): its instance carries model as it is,
    // which the vertex shader turns to face the camera, and no normal matrix.
    bool billboard = false;
    // It may cast a shadow (MeshDraw::castsShadow).
    bool castsShadow = false;
    // Its Material's surface Shader's id (MeshDraw::surface), 0 for none:
    // opaque draws sort by it, so a pass changes program once per Shader, and
    // never join a run across it.
    std::uint64_t program = 0;
    // Its surface Shader says render_mode transparent: see-through whatever
    // its transparency.
    bool seeThrough = false;
    // How far its surface Shader's vertex stage may move a vertex, in world
    // units: its culling sphere grows by it. 0 without a vertex stage.
    float margin = 0.f;
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

// VisibilityResult::fades' value for a draw that is not fading.
constexpr float kLodShown = 1.f;

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
    // One per DrawItem, set by CullDraws: a banded one's fade (StepLodFade),
    // 1 for any other. NaN before its first cull.
    std::vector<float> fades;
    // The part of a whole fade CullDraws moves each banded draw on by: 1 jumps
    // every fade to its end, as a band's end did before fades.
    float fadeStep = 1.f;
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

// Seconds an LOD takes to dither in, or out, once the camera crosses its band's end.
constexpr float kLodFadeSeconds = 0.4f;

// A banded draw's fade: how much of it is drawn, signed by whether the camera
// is in its band. Above 0 it is in, and fading in until 1; at or below 0 it
// is out, and fading out until 0. The draws on both sides of a band's end
// start their fades together, so one is always 1 minus the other, and the
// fragment shaders' dither (InstanceData::fade) gives each the pixels the
// other leaves. step is the part of a whole fade since the last; NaN, as for
// a draw never seen, jumps to the end.
inline float StepLodFade(float fade, bool inside, float step) {
    if (std::isnan(fade)) {
        return inside ? 1.f : 0.f;
    }
    const float shown = std::abs(fade);
    return inside ? std::min(1.f, shown + step) : -std::max(0.f, shown - step);
}

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
