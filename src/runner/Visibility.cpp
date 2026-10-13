#include "Visibility.hpp"

#include "ParallelBlocks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace runner {

namespace {

// ScreenRadius with tan(fovY / 2) precomputed once per FindVisible call,
// instead of once per draw.
float ScreenRadiusScaled(const Sphere& sphere, const CameraView& camera, float tanHalfFov) {
    const float* eye = camera.world.m + 12;
    const float dx = sphere.center.x - eye[0];
    const float dy = sphere.center.y - eye[1];
    const float dz = sphere.center.z - eye[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= sphere.radius) {
        return std::numeric_limits<float>::infinity();
    }
    return sphere.radius * static_cast<float>(camera.paneHeight) / (2.f * distance * tanHalfFov);
}

}  // namespace

float ScreenRadius(const Sphere& sphere, const CameraView& camera) {
    const float halfFov = camera.fovYDegrees * 0.5f * 0.01745329252f;
    return ScreenRadiusScaled(sphere, camera, std::tan(halfFov));
}

void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out,
                 std::size_t grain) {
    const auto total = static_cast<std::size_t>(std::max(count, 0));
    // Every sphere is written below, so the old ones need no clearing.
    out.spheres.resize(total);
    out.opaque.clear();
    out.transparent.clear();
    out.culled = 0;
    const Frustum frustum = MakeFrustum(camera.viewProjection);
    const float halfFov = camera.fovYDegrees * 0.5f * 0.01745329252f;
    const float tanHalfFov = std::tan(halfFov);
    const std::size_t blockCount = BlockCount(total, grain);
    const bool alone = blockCount == 1;
    if (!alone) {
        out.blocks.resize(blockCount);
    }
    ParallelBlocks(total, grain, [&](std::size_t block, std::size_t begin, std::size_t end) {
        // One block writes straight into the result.
        VisibilityResult::Block* mine = alone ? nullptr : &out.blocks[block];
        std::vector<VisibleDraw>& opaque = alone ? out.opaque : mine->opaque;
        std::vector<VisibleDraw>& transparent = alone ? out.transparent : mine->transparent;
        opaque.clear();
        transparent.clear();
        int culled = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const DrawItem& item = items[index];
            if (!item.drawable) {
                out.spheres[index] = Sphere{};
                continue;
            }
            const Sphere sphere = WorldBounds(*item.model, item.boundsMin, item.boundsMax);
            out.spheres[index] = sphere;
            if (item.shadowOnly) {
                continue;
            }
            if (cull && !SphereInFrustum(frustum, sphere)) {
                ++culled;
                continue;
            }
            VisibleDraw visible;
            visible.index = static_cast<int>(index);
            visible.screenRadius = ScreenRadiusScaled(sphere, camera, tanHalfFov);
            visible.lod = item.lod;
            (item.transparency > 0.f ? transparent : opaque).push_back(visible);
        }
        if (alone) {
            out.culled = culled;
        } else {
            mine->culled = culled;
        }
    });
    if (alone) {
        return;
    }
    for (const VisibilityResult::Block& block : out.blocks) {
        out.opaque.insert(out.opaque.end(), block.opaque.begin(), block.opaque.end());
        out.transparent.insert(out.transparent.end(), block.transparent.begin(), block.transparent.end());
        out.culled += block.culled;
    }
}

}  // namespace runner
