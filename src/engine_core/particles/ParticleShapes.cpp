#include "particles/ParticleShapes.hpp"

#include "particles/ParticleMath.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::particles {
namespace {

constexpr float kPi = 3.14159265358979f;

float arc_angle(float arc_degrees, Pcg32& rng) {
    return rng.uniform() * std::clamp(arc_degrees, 0.f, 360.f) * kPi / 180.f;
}

float random_sign(Pcg32& rng) { return rng.uniform() < 0.5f ? -1.f : 1.f; }

// The point at angle a on the X/Z ellipse (hx, hz), r of the way out.
Vec3 ring(float a, float hx, float hz, float r) { return {std::cos(a) * hx * r, 0.f, std::sin(a) * hz * r}; }

// The outward normal of the axis-aligned ellipsoid h at its surface point p.
Vec3 ellipsoid_normal(Vec3 p, Vec3 h) {
    const auto inv2 = [](float v) { return v > 0.f ? 1.f / (v * v) : 0.f; };
    return normalize(Vec3{p.x * inv2(h.x), p.y * inv2(h.y), p.z * inv2(h.z)});
}

const Vec3 kUp{0.f, 1.f, 0.f};

ShapeSample box(ParticleEmitFrom from, Vec3 h, Pcg32& rng) {
    if (from == ParticleEmitFrom::Volume) {
        return {{rng.range(-h.x, h.x), rng.range(-h.y, h.y), rng.range(-h.z, h.z)}, kUp};
    }
    if (from == ParticleEmitFrom::Surface) {
        // A face pair by its area, then a side of it.
        const float ax = h.y * h.z;
        const float ay = h.x * h.z;
        const float az = h.x * h.y;
        float pick = rng.uniform() * (ax + ay + az);
        const float s = random_sign(rng);
        if ((pick -= ax) < 0.f) {
            return {{s * h.x, rng.range(-h.y, h.y), rng.range(-h.z, h.z)}, {s, 0.f, 0.f}};
        }
        if ((pick -= ay) < 0.f) {
            return {{rng.range(-h.x, h.x), s * h.y, rng.range(-h.z, h.z)}, {0.f, s, 0.f}};
        }
        return {{rng.range(-h.x, h.x), rng.range(-h.y, h.y), s * h.z}, {0.f, 0.f, s}};
    }
    // Edges: four along each axis, picked by length.
    float pick = rng.uniform() * (h.x + h.y + h.z);
    const float s1 = random_sign(rng);
    const float s2 = random_sign(rng);
    Vec3 p;
    if ((pick -= h.x) < 0.f) {
        p = {rng.range(-h.x, h.x), s1 * h.y, s2 * h.z};
    } else if ((pick -= h.y) < 0.f) {
        p = {s1 * h.x, rng.range(-h.y, h.y), s2 * h.z};
    } else {
        p = {s1 * h.x, s2 * h.y, rng.range(-h.z, h.z)};
    }
    return {p, normalize(p)};
}

// Uniform over the sphere: longitude within the arc, height uniform
// (Archimedes), the upper half only for a hemisphere.
ShapeSample sphere(ParticleEmitFrom from, Vec3 h, float arc, bool hemi, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    if (from == ParticleEmitFrom::Edge) {
        // The equator, which is also a hemisphere's rim.
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {p, normalize(Vec3{std::cos(a), 0.f, std::sin(a)})};
    }
    const float y = hemi ? rng.uniform() : rng.range(-1.f, 1.f);
    const float across = std::sqrt(std::max(0.f, 1.f - y * y));
    const Vec3 dir{std::cos(a) * across, y, std::sin(a) * across};
    const Vec3 surface{dir.x * h.x, dir.y * h.y, dir.z * h.z};
    const Vec3 normal = ellipsoid_normal(surface, h);
    if (from == ParticleEmitFrom::Surface) {
        return {surface, normal};
    }
    return {scale(surface, std::cbrt(rng.uniform())), normal};
}

ShapeSample cylinder(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    if (from == ParticleEmitFrom::Volume) {
        const Vec3 p = ring(a, h.x, h.z, std::sqrt(rng.uniform()));
        return {{p.x, rng.range(-h.y, h.y), p.z}, kUp};
    }
    if (from == ParticleEmitFrom::Edge) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        const float y = random_sign(rng) * h.y;
        return {{p.x, y, p.z}, normalize(Vec3{std::cos(a), y > 0.f ? 1.f : -1.f, std::sin(a)})};
    }
    // The side by its area (an ellipse's perimeter is about pi(a + b)) against the two caps.
    const float side = kPi * (h.x + h.z) * 2.f * h.y;
    const float caps = 2.f * kPi * h.x * h.z;
    if (rng.uniform() * (side + caps) < side) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {{p.x, rng.range(-h.y, h.y), p.z}, normalize(Vec3{std::cos(a), 0.f, std::sin(a)})};
    }
    const Vec3 p = ring(a, h.x, h.z, std::sqrt(rng.uniform()));
    const float s = random_sign(rng);
    return {{p.x, s * h.y, p.z}, {0.f, s, 0.f}};
}

// Apex at -h.y, the wide base at +h.y. k runs 0 at the apex to 1 at the base.
ShapeSample cone(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    const float radius = 0.5f * (h.x + h.z);
    // Outward on the slanted side: out by the height, down by the radius.
    const Vec3 side_normal = normalize(Vec3{std::cos(a) * 2.f * h.y, -radius, std::sin(a) * 2.f * h.y});
    if (from == ParticleEmitFrom::Edge) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {{p.x, h.y, p.z}, side_normal};
    }
    if (from == ParticleEmitFrom::Surface) {
        const float k = std::sqrt(rng.uniform());   // the side's area grows with k
        const Vec3 p = ring(a, h.x, h.z, k);
        return {{p.x, -h.y + 2.f * h.y * k, p.z}, side_normal};
    }
    const float k = std::cbrt(rng.uniform());       // the volume grows with k squared
    const Vec3 p = ring(a, h.x, h.z, k * std::sqrt(rng.uniform()));
    return {{p.x, -h.y + 2.f * h.y * k, p.z}, kUp};
}

ShapeSample disc(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    if (from == ParticleEmitFrom::Edge) {
        return {ring(a, h.x, h.z, 1.f), normalize(Vec3{std::cos(a), 0.f, std::sin(a)})};
    }
    return {ring(a, h.x, h.z, std::sqrt(rng.uniform())), kUp};
}

}  // namespace

ShapeSample sample_shape(ParticleShape shape, ParticleEmitFrom from, Vec3 half, float arc_degrees, Pcg32& rng) {
    if (!(half.x > 0.f) && !(half.y > 0.f) && !(half.z > 0.f)) {
        return {{0.f, 0.f, 0.f}, kUp};
    }
    switch (shape) {
    case ParticleShape::Point:
        return {{0.f, 0.f, 0.f}, kUp};
    case ParticleShape::Box:
        return box(from, half, rng);
    case ParticleShape::Sphere:
        return sphere(from, half, arc_degrees, false, rng);
    case ParticleShape::Hemisphere:
        return sphere(from, half, arc_degrees, true, rng);
    case ParticleShape::Cylinder:
        return cylinder(from, half, arc_degrees, rng);
    case ParticleShape::Cone:
        return cone(from, half, arc_degrees, rng);
    case ParticleShape::Disc:
        return disc(from, half, arc_degrees, rng);
    }
    return {{0.f, 0.f, 0.f}, kUp};
}

Vec3 spread_direction(Vec3 dir, float spread_x_deg, float spread_y_deg, Pcg32& rng) {
    const Vec3 d = normalize(dir);
    if (!(spread_x_deg > 0.f) && !(spread_y_deg > 0.f)) {
        return d;
    }
    // The axes each tilt turns about: for +Y, Z (toward X) and then X (toward Z).
    const Vec3 x_axis = std::fabs(d.y) > 0.9f ? Vec3{0.f, 0.f, 1.f} : normalize(cross(d, Vec3{0.f, 1.f, 0.f}));
    const Vec3 y_axis = normalize(cross(x_axis, d));
    const float ax = rng.range(-spread_x_deg, spread_x_deg) * kPi / 180.f;
    const float ay = rng.range(-spread_y_deg, spread_y_deg) * kPi / 180.f;
    return normalize(rotate(rotate(d, x_axis, ax), y_axis, ay));
}

}  // namespace engine_core::particles
