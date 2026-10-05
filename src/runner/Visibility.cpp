#include "Visibility.hpp"

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

void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out) {
    count = std::max(count, 0);
    out.spheres.assign(static_cast<std::size_t>(count), Sphere{});
    out.opaque.clear();
    out.transparent.clear();
    out.culled = 0;
    const Frustum frustum = MakeFrustum(camera.viewProjection);
    const float halfFov = camera.fovYDegrees * 0.5f * 0.01745329252f;
    const float tanHalfFov = std::tan(halfFov);
    for (int index = 0; index < count; ++index) {
        const DrawItem& item = items[index];
        if (!item.drawable) {
            continue;
        }
        const Sphere sphere = WorldBounds(*item.model, item.boundsMin, item.boundsMax);
        out.spheres[static_cast<std::size_t>(index)] = sphere;
        if (cull && !SphereInFrustum(frustum, sphere)) {
            ++out.culled;
            continue;
        }
        VisibleDraw visible;
        visible.index = index;
        visible.screenRadius = ScreenRadiusScaled(sphere, camera, tanHalfFov);
        (item.transparency > 0.f ? out.transparent : out.opaque).push_back(visible);
    }
}

}  // namespace runner
