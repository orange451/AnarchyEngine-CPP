#include "Visibility.hpp"

#include <atomic>

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

void NormalizeRanges(std::vector<DrawRange>& ranges) {
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](const DrawRange& range) { return range.count == 0; }),
                 ranges.end());
    std::sort(ranges.begin(), ranges.end(),
              [](const DrawRange& a, const DrawRange& b) { return a.first < b.first; });
    std::size_t kept = 0;
    for (const DrawRange& range : ranges) {
        if (kept > 0 && ranges[kept - 1].first + ranges[kept - 1].count >= range.first) {
            DrawRange& last = ranges[kept - 1];
            last.count = std::max(last.first + last.count, range.first + range.count) - last.first;
        } else {
            ranges[kept++] = range;
        }
    }
    ranges.resize(kept);
}

namespace {

void BoundItem(const DrawItem& item, Sphere& sphere, std::uint16_t& traits, VisibilityResult::Band& band) {
    if (!item.drawable) {
        sphere = Sphere{};
        traits = 0;
        return;
    }
    sphere = WorldBounds(*item.model, item.boundsMin, item.boundsMax);
    const bool banded = item.lodNear > 0.f || item.lodFar < std::numeric_limits<float>::infinity();
    traits = static_cast<std::uint16_t>(kTraitDrawable | (item.shadowOnly ? kTraitShadowOnly : 0) |
                                        (item.transparency > 0.f ? kTraitTransparent : 0) |
                                        (banded ? kTraitBanded : 0) | (item.castsShadow ? kTraitCasts : 0) |
                                        (item.lod << 8));
    if (banded) {
        const float* origin = item.model->m + 12;
        band.x = origin[0];
        band.y = origin[1];
        band.z = origin[2];
        const float keptNear = LodKeptNear(item.lodNear);
        const float keptFar = LodKeptFar(item.lodFar);
        band.nearSquared = keptNear * keptNear;
        band.farSquared = keptFar * keptFar;
    }
}

}  // namespace

void UpdateDrawBounds(const DrawItem* items, int count, bool all, const std::vector<DrawRange>& ranges,
                      VisibilityResult& out, std::size_t grain) {
    const auto total = static_cast<std::size_t>(std::max(count, 0));
    out.spheres.resize(total);
    out.traits.resize(total);
    out.bands.resize(total);
    out.outOfBand.resize(total);
    const auto bound = [&](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            BoundItem(items[index], out.spheres[index], out.traits[index], out.bands[index]);
        }
    };
    if (all) {
        ParallelBlocks(total, grain, [&](std::size_t, std::size_t begin, std::size_t end) { bound(begin, end); });
        return;
    }
    ParallelRanges(ranges, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
        bound(begin, std::min(end, total));
    });
}

void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out,
                 std::size_t grain) {
    UpdateDrawBounds(items, count, true, {}, out, grain);
    CullDraws(count, camera, cull, out, grain);
}

void CullDraws(int count, const CameraView& camera, bool cull, VisibilityResult& out, std::size_t grain) {
    const auto total = static_cast<std::size_t>(std::max(count, 0));
    out.opaque.clear();
    out.transparent.clear();
    out.casters.clear();
    out.culled = 0;
    if (out.outOfBand.size() != total) {
        out.outOfBand.resize(total);
        ++out.outOfBandRevision;
    }
    std::atomic<bool> flipped{false};
    const float* eye = camera.world.m + 12;
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
        std::vector<int>& casters = alone ? out.casters : mine->casters;
        opaque.clear();
        transparent.clear();
        casters.clear();
        int culled = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const std::uint16_t traits = out.traits[index];
            // Outside its band, whether or not culling is on: neither seen nor casting.
            std::uint8_t& outside = out.outOfBand[index];
            std::uint8_t now = 0;
            if ((traits & kTraitBanded) != 0) {
                const VisibilityResult::Band& band = out.bands[index];
                const float dx = band.x - eye[0];
                const float dy = band.y - eye[1];
                const float dz = band.z - eye[2];
                const float squared = dx * dx + dy * dy + dz * dz;
                now = squared < band.nearSquared || !(squared < band.farSquared) ? 1 : 0;
            }
            if (now != outside) {
                outside = now;
                flipped.store(true, std::memory_order_relaxed);
            }
            if (now != 0) {
                continue;
            }
            // In view or not: a caster outside the view can shade what is in it.
            if ((traits & (kTraitDrawable | kTraitCasts)) == (kTraitDrawable | kTraitCasts)) {
                casters.push_back(static_cast<int>(index));
            }
            if ((traits & kTraitDrawable) == 0 || (traits & kTraitShadowOnly) != 0) {
                continue;
            }
            const Sphere& sphere = out.spheres[index];
            if (cull && !SphereInFrustum(frustum, sphere)) {
                ++culled;
                continue;
            }
            VisibleDraw visible;
            visible.index = static_cast<int>(index);
            visible.screenRadius = ScreenRadiusScaled(sphere, camera, tanHalfFov);
            visible.lod = static_cast<std::uint8_t>(traits >> 8);
            ((traits & kTraitTransparent) != 0 ? transparent : opaque).push_back(visible);
        }
        if (alone) {
            out.culled = culled;
        } else {
            mine->culled = culled;
        }
    });
    if (flipped.load(std::memory_order_relaxed)) {
        ++out.outOfBandRevision;
    }
    if (alone) {
        return;
    }
    for (const VisibilityResult::Block& block : out.blocks) {
        out.opaque.insert(out.opaque.end(), block.opaque.begin(), block.opaque.end());
        out.transparent.insert(out.transparent.end(), block.transparent.begin(), block.transparent.end());
        out.casters.insert(out.casters.end(), block.casters.begin(), block.casters.end());
        out.culled += block.culled;
    }
}

}  // namespace runner
