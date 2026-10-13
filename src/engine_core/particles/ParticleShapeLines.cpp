#include "particles/ParticleShapeLines.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::particles {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kSegments = 32;
constexpr float kColor[4] = {1.f, 0.75f, 0.2f, 1.f};

struct Lines {
    const Matrix4& frame;
    std::vector<float>& out;

    void point(Vec3 p) {
        const Vec3 w = matrix4_point(frame, p);
        out.insert(out.end(), {w.x, w.y, w.z, kColor[0], kColor[1], kColor[2], kColor[3]});
    }
    void segment(Vec3 a, Vec3 b) {
        point(a);
        point(b);
    }
    // The ellipse in the X/Z plane at height y, through arc, radii (hx, hz).
    void ring(float y, float hx, float hz, float arc) {
        const int steps = std::max(1, static_cast<int>(std::ceil(kSegments * arc / (2.f * kPi))));
        for (int i = 0; i < steps; ++i) {
            const float a0 = arc * static_cast<float>(i) / static_cast<float>(steps);
            const float a1 = arc * static_cast<float>(i + 1) / static_cast<float>(steps);
            segment({std::cos(a0) * hx, y, std::sin(a0) * hz}, {std::cos(a1) * hx, y, std::sin(a1) * hz});
        }
    }
    // An upright ellipse through the Y axis, along the X/Z direction at
    // angle a, over latitudes low to high (radians).
    void meridian(float a, Vec3 h, float low, float high) {
        for (int i = 0; i < kSegments / 2; ++i) {
            const float t0 = low + (high - low) * static_cast<float>(i) / (kSegments / 2.f);
            const float t1 = low + (high - low) * static_cast<float>(i + 1) / (kSegments / 2.f);
            const auto at = [&](float t) {
                return Vec3{std::cos(t) * std::cos(a) * h.x, std::sin(t) * h.y, std::cos(t) * std::sin(a) * h.z};
            };
            segment(at(t0), at(t1));
        }
    }
};

}  // namespace

void particle_shape_lines(ParticleShape shape, float arc_degrees, const Matrix4& frame, Vec3 half,
                          std::vector<float>& out) {
    Lines lines{frame, out};
    const Vec3 h = half;
    if (shape == ParticleShape::Point || (!(h.x > 0.f) && !(h.y > 0.f) && !(h.z > 0.f))) {
        const float s = 0.25f;
        lines.segment({-s, 0.f, 0.f}, {s, 0.f, 0.f});
        lines.segment({0.f, -s, 0.f}, {0.f, s, 0.f});
        lines.segment({0.f, 0.f, -s}, {0.f, 0.f, s});
        return;
    }
    const float arc = std::clamp(arc_degrees, 0.f, 360.f) * kPi / 180.f;
    switch (shape) {
    case ParticleShape::Box:
        for (const float a : {-1.f, 1.f}) {
            for (const float b : {-1.f, 1.f}) {
                lines.segment({-h.x, a * h.y, b * h.z}, {h.x, a * h.y, b * h.z});
                lines.segment({a * h.x, -h.y, b * h.z}, {a * h.x, h.y, b * h.z});
                lines.segment({a * h.x, b * h.y, -h.z}, {a * h.x, b * h.y, h.z});
            }
        }
        break;
    case ParticleShape::Sphere:
        lines.ring(0.f, h.x, h.z, arc);
        lines.meridian(0.f, h, -kPi / 2.f, kPi / 2.f);
        lines.meridian(std::min(arc, kPi / 2.f), h, -kPi / 2.f, kPi / 2.f);
        break;
    case ParticleShape::Hemisphere:
        lines.ring(0.f, h.x, h.z, arc);
        lines.meridian(0.f, h, 0.f, kPi / 2.f);
        lines.meridian(std::min(arc, kPi / 2.f), h, 0.f, kPi / 2.f);
        break;
    case ParticleShape::Cylinder:
        lines.ring(-h.y, h.x, h.z, arc);
        lines.ring(h.y, h.x, h.z, arc);
        for (int i = 0; i < 4; ++i) {
            const float a = arc * static_cast<float>(i) / 4.f;
            lines.segment({std::cos(a) * h.x, -h.y, std::sin(a) * h.z}, {std::cos(a) * h.x, h.y, std::sin(a) * h.z});
        }
        break;
    case ParticleShape::Cone:
        lines.ring(h.y, h.x, h.z, arc);
        for (int i = 0; i < 4; ++i) {
            const float a = arc * static_cast<float>(i) / 4.f;
            lines.segment({0.f, -h.y, 0.f}, {std::cos(a) * h.x, h.y, std::sin(a) * h.z});
        }
        break;
    case ParticleShape::Disc:
        lines.ring(0.f, h.x, h.z, arc);
        break;
    case ParticleShape::Point:
        break;
    }
}

}  // namespace engine_core::particles
