#include "MeshShapes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine_core {

namespace {

using anarchy::amesh::Data;
using anarchy::amesh::Vertex;

constexpr float kPi = 3.14159265358979f;

struct V3 {
    float x = 0, y = 0, z = 0;
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalized(V3 v) {
    const float length = std::sqrt(dot(v, v));
    return length > 0.f ? v * (1.f / length) : v;
}
V3 to_v3(Vec3 v) { return {v.x, v.y, v.z}; }

int clamp_segments(int segments) { return std::clamp(segments, kMinShapeSegments, kMaxShapeSegments); }

std::uint32_t push(Data& data, V3 p, V3 n, float u, float v) {
    Vertex vertex;
    vertex.p[0] = p.x, vertex.p[1] = p.y, vertex.p[2] = p.z;
    vertex.n[0] = n.x, vertex.n[1] = n.y, vertex.n[2] = n.z;
    vertex.uv[0] = u, vertex.uv[1] = v;
    data.vertices.push_back(vertex);
    return static_cast<std::uint32_t>(data.vertices.size() - 1);
}

V3 position_of(const Data& data, std::uint32_t index) {
    const float* p = data.vertices[index].p;
    return {p[0], p[1], p[2]};
}

V3 normal_of(const Data& data, std::uint32_t index) {
    const float* n = data.vertices[index].n;
    return {n[0], n[1], n[2]};
}

// A triangle wound CCW as seen from the side its vertex normals face, so no
// shape can come out inside-out. One with no area (at a pole) is left out.
void triangle(Data& data, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const V3 pa = position_of(data, a);
    const V3 face = cross(position_of(data, b) - pa, position_of(data, c) - pa);
    if (!(dot(face, face) > 1e-20f)) {
        return;
    }
    const V3 outward = normal_of(data, a) + normal_of(data, b) + normal_of(data, c);
    if (dot(face, outward) < 0.f) {
        std::swap(b, c);
    }
    data.indices.insert(data.indices.end(), {a, b, c});
}

void quad(Data& data, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    triangle(data, a, b, c);
    triangle(data, a, c, d);
}

// One (radius, height) point of an outline that is turned about the Y axis.
struct ProfilePoint {
    float r = 0;
    float y = 0;
};

// Turns profile about Y, segments around, with normals smooth along the
// profile. The profile runs in order along the surface; its outward side is
// to the right of that direction in the (r, y) plane. With round_poles, an
// end on the axis gets a normal straight along it, as a sphere's poles do.
void lathe(Data& data, const std::vector<ProfilePoint>& profile, int segments, V3 at, float scale,
           bool round_poles) {
    const std::size_t count = profile.size();
    if (count < 2) {
        return;
    }
    std::vector<float> along(count, 0.f);
    for (std::size_t i = 1; i < count; ++i) {
        along[i] = along[i - 1] + std::hypot(profile[i].r - profile[i - 1].r, profile[i].y - profile[i - 1].y);
    }
    const float length = along.back() > 0.f ? along.back() : 1.f;
    const auto first = static_cast<std::uint32_t>(data.vertices.size());
    for (std::size_t i = 0; i < count; ++i) {
        const ProfilePoint& before = profile[i == 0 ? 0 : i - 1];
        const ProfilePoint& after = profile[i + 1 == count ? i : i + 1];
        float nr = after.y - before.y;
        float ny = -(after.r - before.r);
        const float n_length = std::hypot(nr, ny);
        if (n_length > 0.f) {
            nr /= n_length;
            ny /= n_length;
        }
        if (round_poles && profile[i].r == 0.f && (i == 0 || i + 1 == count)) {
            nr = 0.f;
            ny = ny < 0.f ? -1.f : 1.f;
        }
        for (int j = 0; j <= segments; ++j) {
            const float angle = 2.f * kPi * static_cast<float>(j) / static_cast<float>(segments);
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const V3 p{profile[i].r * c * scale, profile[i].y * scale, profile[i].r * s * scale};
            push(data, at + p, V3{nr * c, ny, nr * s}, static_cast<float>(j) / static_cast<float>(segments),
                 along[i] / length);
        }
    }
    const auto row = static_cast<std::uint32_t>(segments + 1);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(segments); ++j) {
            const std::uint32_t a = first + static_cast<std::uint32_t>(i) * row + j;
            const std::uint32_t b = a + row;
            quad(data, a, a + 1, b + 1, b);
        }
    }
}

// A flat disk of radius at height y, facing +Y when up, else -Y.
void disk(Data& data, float radius, float y, bool up, int segments, V3 at) {
    const V3 normal{0.f, up ? 1.f : -1.f, 0.f};
    const std::uint32_t center = push(data, at + V3{0.f, y, 0.f}, normal, 0.5f, 0.5f);
    const auto first = static_cast<std::uint32_t>(data.vertices.size());
    for (int j = 0; j <= segments; ++j) {
        const float angle = 2.f * kPi * static_cast<float>(j) / static_cast<float>(segments);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        push(data, at + V3{radius * c, y, radius * s}, normal, 0.5f + 0.5f * c, 0.5f + 0.5f * s);
    }
    for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(segments); ++j) {
        triangle(data, center, first + j, first + j + 1);
    }
}

V3 bezier(const V3 (&p)[4], float t) {
    const float u = 1.f - t;
    return p[0] * (u * u * u) + p[1] * (3.f * u * u * t) + p[2] * (3.f * u * t * t) + p[3] * (t * t * t);
}

V3 bezier_tangent(const V3 (&p)[4], float t) {
    const float u = 1.f - t;
    return (p[1] - p[0]) * (3.f * u * u) + (p[2] - p[1]) * (6.f * u * t) + (p[3] - p[2]) * (3.f * t * t);
}

// A round tube along a cubic Bezier that lies in the XY plane, its radius
// running from start_radius to end_radius. Its ends are open.
void tube(Data& data, const V3 (&curve)[4], float start_radius, float end_radius, int steps, int segments,
          V3 at, float scale) {
    const V3 across{0.f, 0.f, 1.f};
    const auto first = static_cast<std::uint32_t>(data.vertices.size());
    for (int k = 0; k <= steps; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const V3 center = bezier(curve, t);
        const V3 side = normalized(cross(across, normalized(bezier_tangent(curve, t))));
        const float radius = start_radius + (end_radius - start_radius) * t;
        for (int j = 0; j <= segments; ++j) {
            const float angle = 2.f * kPi * static_cast<float>(j) / static_cast<float>(segments);
            const V3 out = side * std::cos(angle) + across * std::sin(angle);
            push(data, at + (center + out * radius) * scale, out, static_cast<float>(j) / static_cast<float>(segments), t);
        }
    }
    const auto row = static_cast<std::uint32_t>(segments + 1);
    for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(steps); ++k) {
        for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(segments); ++j) {
            const std::uint32_t a = first + k * row + j;
            quad(data, a, a + 1, a + row + 1, a + row);
        }
    }
}

}  // namespace

void add_box(Data& data, Vec3 size, Vec3 at) {
    const V3 half{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    const V3 center = to_v3(at);
    // Each face: its normal and two axes across it.
    const V3 faces[6][3] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const auto& face : faces) {
        const V3 n = face[0];
        const V3 u = face[1];
        const V3 v = face[2];
        auto corner = [&](float a, float b) {
            const V3 offset = n + u * a + v * b;
            return center + V3{offset.x * half.x, offset.y * half.y, offset.z * half.z};
        };
        const std::uint32_t a = push(data, corner(-1, -1), n, 0.f, 0.f);
        const std::uint32_t b = push(data, corner(1, -1), n, 1.f, 0.f);
        const std::uint32_t c = push(data, corner(1, 1), n, 1.f, 1.f);
        const std::uint32_t d = push(data, corner(-1, 1), n, 0.f, 1.f);
        quad(data, a, b, c, d);
    }
}

void add_sphere(Data& data, float radius, int segments, Vec3 at) {
    segments = clamp_segments(segments);
    const int rings = std::max(2, segments / 2);
    std::vector<ProfilePoint> profile;
    for (int i = 0; i <= rings; ++i) {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(rings);
        profile.push_back({radius * std::sin(phi), -radius * std::cos(phi)});
    }
    lathe(data, profile, segments, to_v3(at), 1.f, true);
}

void add_cylinder(Data& data, float radius, float height, int segments, bool capped, Vec3 at) {
    segments = clamp_segments(segments);
    const float half = height * 0.5f;
    lathe(data, {{radius, -half}, {radius, half}}, segments, to_v3(at), 1.f, false);
    if (capped) {
        disk(data, radius, -half, false, segments, to_v3(at));
        disk(data, radius, half, true, segments, to_v3(at));
    }
}

void add_cone(Data& data, float radius, float height, int segments, bool capped, Vec3 at) {
    segments = clamp_segments(segments);
    const float half = height * 0.5f;
    lathe(data, {{radius, -half}, {0.f, half}}, segments, to_v3(at), 1.f, false);
    if (capped) {
        disk(data, radius, -half, false, segments, to_v3(at));
    }
}

void add_plane(Data& data, float width, float depth, Vec3 at) {
    const V3 center = to_v3(at);
    const V3 up{0.f, 1.f, 0.f};
    const float x = width * 0.5f;
    const float z = depth * 0.5f;
    const std::uint32_t a = push(data, center + V3{-x, 0.f, z}, up, 0.f, 0.f);
    const std::uint32_t b = push(data, center + V3{x, 0.f, z}, up, 1.f, 0.f);
    const std::uint32_t c = push(data, center + V3{x, 0.f, -z}, up, 1.f, 1.f);
    const std::uint32_t d = push(data, center + V3{-x, 0.f, -z}, up, 0.f, 1.f);
    quad(data, a, b, c, d);
}

void add_teapot(Data& data, float size, Vec3 at) {
    // Newell's teapot stands 3.15 units tall, from its base to the top of the knob.
    constexpr float kTall = 3.15f;
    const float scale = size / kTall;
    const V3 base = to_v3(at);
    constexpr int kAround = 32;
    disk(data, 1.5f * scale, 0.f, false, kAround, base);
    // Body, from the base's edge up to the rim.
    lathe(data,
          {{1.50f, 0.00f}, {1.72f, 0.15f}, {1.87f, 0.30f}, {1.96f, 0.45f}, {2.00f, 0.75f}, {1.98f, 1.05f},
           {1.90f, 1.35f}, {1.78f, 1.65f}, {1.63f, 1.95f}, {1.47f, 2.25f}, {1.40f, 2.40f}, {1.44f, 2.46f},
           {1.42f, 2.50f}},
          kAround, base, scale, false);
    // Lid, sitting inside the rim, up over the knob to the top.
    lathe(data,
          {{1.32f, 2.44f}, {1.20f, 2.52f}, {0.90f, 2.60f}, {0.50f, 2.66f}, {0.20f, 2.70f}, {0.30f, 2.78f},
           {0.42f, 2.90f}, {0.40f, 3.02f}, {0.25f, 3.11f}, {0.00f, 3.15f}},
          kAround, base, scale, true);
    // Spout, out of the lower body toward +X and up to its mouth.
    const V3 spout[4] = {{1.60f, 0.90f, 0.f}, {2.60f, 0.90f, 0.f}, {2.80f, 1.90f, 0.f}, {3.30f, 2.40f, 0.f}};
    tube(data, spout, 0.45f, 0.18f, 24, 16, base, scale);
    // Handle, a loop out toward -X, both ends in the body.
    const V3 handle[4] = {{-1.50f, 2.10f, 0.f}, {-2.90f, 2.35f, 0.f}, {-3.00f, 0.85f, 0.f}, {-1.75f, 0.70f, 0.f}};
    tube(data, handle, 0.14f, 0.14f, 24, 12, base, scale);
}

}  // namespace engine_core
