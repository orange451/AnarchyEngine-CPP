#include "brush/BrushGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace engine_core::brush {

double dot(DVec3 a, DVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

DVec3 cross(DVec3 a, DVec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double length(DVec3 a) { return std::sqrt(dot(a, a)); }

DVec3 normalize(DVec3 a) {
    const double l = length(a);
    return l > 0.0 ? a / l : DVec3{};
}

bool operator==(const Face& a, const Face& b) {
    return a.p1 == b.p1 && a.p2 == b.p2 && a.p3 == b.p3 && a.material == b.material &&
           a.u_axis == b.u_axis && a.v_axis == b.v_axis && a.offset_u == b.offset_u &&
           a.offset_v == b.offset_v && a.scale_u == b.scale_u && a.scale_v == b.scale_v &&
           a.rotation == b.rotation;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
// Vertices this close are one vertex (the spec's 1e-6).
constexpr double kWeld = 1e-6;
constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

double get(DVec3 v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

double max_norm(DVec3 a) { return std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}); }

// Tolerances for one build, from how far the points reach: clipping works on
// a start box 1000 times their size, so its rounding grows with them.
struct Tolerances {
    double clip;    // a point this close to a plane is on it
    double weld;    // vertices this close are one
    double refine;  // how far solving a vertex's planes may move it
};

Tolerances tolerances_for(double scale) {
    Tolerances t;
    t.clip = 1e-9 * std::max(1.0, scale);
    t.weld = std::max(kWeld, 10.0 * t.clip);
    t.refine = 100.0 * t.weld;
    return t;
}

// A polygon while clipping: its points by value and its face (negative for
// the start box's own sides).
struct WorkPoly {
    int face = 0;
    std::vector<DVec3> points;
};

enum class Cut { kNothing, kEverything, kSome };

// Buffers kept across clips so a build allocates little.
struct Clipper {
    std::vector<double> distances;
    std::vector<DVec3> scratch;
    std::vector<DVec3> cap;
    std::vector<std::pair<double, DVec3>> sorted;
};

// Keeps the part of the solid where normal . x <= distance, closing it with a
// new polygon for `face`. Says whether the plane cut nothing, everything, or some.
Cut clip_solid(std::vector<WorkPoly>& polys, const Plane& plane, int face, double eps, Clipper& c) {
    c.distances.clear();
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    for (const WorkPoly& poly : polys) {
        for (const DVec3& p : poly.points) {
            const double d = dot(plane.normal, p) - plane.distance;
            c.distances.push_back(d);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    }
    if (hi <= eps) return Cut::kNothing;
    if (lo >= -eps) return Cut::kEverything;

    c.cap.clear();
    std::size_t cursor = 0;
    std::size_t write = 0;
    for (std::size_t i = 0; i < polys.size(); ++i) {
        WorkPoly& poly = polys[i];
        const std::size_t n = poly.points.size();
        const double* d = c.distances.data() + cursor;
        cursor += n;
        c.scratch.clear();
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t k = (j + 1 == n) ? 0 : j + 1;
            const DVec3 cur = poly.points[j];
            if (d[j] <= eps) {
                c.scratch.push_back(cur);
                if (d[j] >= -eps) c.cap.push_back(cur);
            }
            if ((d[j] < -eps && d[k] > eps) || (d[j] > eps && d[k] < -eps)) {
                // Always from the inside point toward the outside one, so the
                // two polygons sharing this edge get bit-identical points.
                const bool cur_in = d[j] < 0.0;
                const DVec3 in = cur_in ? cur : poly.points[k];
                const DVec3 out = cur_in ? poly.points[k] : cur;
                const double din = cur_in ? d[j] : d[k];
                const double dout = cur_in ? d[k] : d[j];
                const DVec3 x = in + (out - in) * (din / (din - dout));
                c.scratch.push_back(x);
                c.cap.push_back(x);
            }
        }
        if (c.scratch.size() >= 3) {
            std::swap(poly.points, c.scratch);
            if (write != i) polys[write] = std::move(poly);
            ++write;
        }
    }
    polys.resize(write);

    if (c.cap.size() >= 3) {
        // Order the cap's points by angle about the normal: counter-clockwise
        // seen from outside, since (a, b, normal) is right-handed.
        DVec3 centre;
        for (const DVec3& p : c.cap) centre = centre + p;
        centre = centre / static_cast<double>(c.cap.size());
        const DVec3 n = plane.normal;
        const DVec3 helper = std::fabs(n.x) < 0.6 ? DVec3{1, 0, 0} : DVec3{0, 1, 0};
        const DVec3 a = normalize(cross(n, helper));
        const DVec3 b = cross(n, a);
        c.sorted.clear();
        for (const DVec3& p : c.cap) {
            const DVec3 q = p - centre;
            c.sorted.emplace_back(std::atan2(dot(q, b), dot(q, a)), p);
        }
        std::sort(c.sorted.begin(), c.sorted.end(),
                  [](const auto& l, const auto& r) { return l.first < r.first; });
        WorkPoly cap;
        cap.face = face;
        for (const auto& entry : c.sorted) {
            if (!cap.points.empty() && max_norm(entry.second - cap.points.back()) <= eps) continue;
            cap.points.push_back(entry.second);
        }
        while (cap.points.size() > 1 && max_norm(cap.points.back() - cap.points.front()) <= eps) {
            cap.points.pop_back();
        }
        if (cap.points.size() >= 3) polys.push_back(std::move(cap));
    }
    return Cut::kSome;
}

// Gives points within `tolerance` of one another one index; `unique` gets the
// first of each. Sorted on x so each point only looks at near neighbours.
void weld(const std::vector<DVec3>& points, double tolerance, std::vector<std::uint32_t>& ids,
          std::vector<DVec3>& unique) {
    std::vector<std::uint32_t> order(points.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t l, std::uint32_t r) { return points[l].x < points[r].x; });
    ids.assign(points.size(), kNone);
    unique.clear();
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const std::uint32_t i = order[oi];
        for (std::size_t oj = oi; oj-- > 0;) {
            const std::uint32_t j = order[oj];
            if (points[i].x - points[j].x > tolerance) break;
            if (max_norm(points[i] - points[j]) <= tolerance) {
                ids[i] = ids[j];
                break;
            }
        }
        if (ids[i] == kNone) {
            ids[i] = static_cast<std::uint32_t>(unique.size());
            unique.push_back(points[i]);
        }
    }
}

// Drops repeated neighbours from a loop, the last against the first too.
void tidy(std::vector<std::uint32_t>& loop) {
    loop.erase(std::unique(loop.begin(), loop.end()), loop.end());
    while (loop.size() > 1 && loop.back() == loop.front()) loop.pop_back();
}

// A vertex of a convex solid sits on at least three faces; one on fewer lies
// in the middle of an edge or face and is dropped, as are loops left with
// fewer than three vertices. Repeated, since each drop can expose another.
void prune(std::vector<Polygon>& loops, std::size_t vertex_count) {
    std::vector<std::uint32_t> count;
    bool changed = true;
    while (changed) {
        changed = false;
        count.assign(vertex_count, 0);
        for (const Polygon& loop : loops) {
            for (std::uint32_t v : loop.vertices) ++count[v];
        }
        for (Polygon& loop : loops) {
            const std::size_t before = loop.vertices.size();
            loop.vertices.erase(std::remove_if(loop.vertices.begin(), loop.vertices.end(),
                                               [&](std::uint32_t v) { return count[v] < 3; }),
                                loop.vertices.end());
            if (loop.vertices.size() != before) changed = true;
        }
        const std::size_t before = loops.size();
        loops.erase(std::remove_if(loops.begin(), loops.end(),
                                   [](const Polygon& l) { return l.vertices.size() < 3; }),
                    loops.end());
        if (loops.size() != before) changed = true;
    }
}

// Moves each vertex to where its three most independent planes meet, which
// undoes clipping's rounding: grid-aligned faces give grid-exact vertices.
void refine(std::vector<DVec3>& vertices, const std::vector<Polygon>& loops,
            const std::vector<Plane>& planes, double max_move) {
    std::vector<std::uint32_t> offsets(vertices.size() + 1, 0);
    for (const Polygon& loop : loops) {
        for (std::uint32_t v : loop.vertices) ++offsets[v + 1];
    }
    for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
    std::vector<std::uint32_t> incident(offsets.back());
    std::vector<std::uint32_t> fill(offsets.begin(), offsets.end() - 1);
    for (const Polygon& loop : loops) {
        for (std::uint32_t v : loop.vertices) incident[fill[v]++] = loop.face;
    }
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        const std::uint32_t first = offsets[v];
        const std::uint32_t last = offsets[v + 1];
        if (last - first < 3) continue;
        const Plane& p0 = planes[incident[first]];
        std::uint32_t best_b = first;
        double best = -1.0;
        for (std::uint32_t i = first + 1; i < last; ++i) {
            const double s = length(cross(p0.normal, planes[incident[i]].normal));
            if (s > best) {
                best = s;
                best_b = i;
            }
        }
        const Plane& pb = planes[incident[best_b]];
        const DVec3 ab = cross(p0.normal, pb.normal);
        std::uint32_t best_c = first;
        best = -1.0;
        for (std::uint32_t i = first + 1; i < last; ++i) {
            const double s = std::fabs(dot(ab, planes[incident[i]].normal));
            if (s > best) {
                best = s;
                best_c = i;
            }
        }
        const Plane& pc = planes[incident[best_c]];
        const double det = dot(p0.normal, cross(pb.normal, pc.normal));
        if (std::fabs(det) < 1e-12) continue;
        const DVec3 x = (cross(pb.normal, pc.normal) * p0.distance + cross(pc.normal, p0.normal) * pb.distance +
                         ab * pc.distance) /
                        det;
        if (max_norm(x - vertices[v]) <= max_move) vertices[v] = x;
    }
}

// The clipped polygons as shared vertices and index loops (faces as given).
struct Solid {
    std::vector<DVec3> vertices;
    std::vector<Polygon> loops;
};

void make_loops(const std::vector<WorkPoly>& polys, const std::vector<std::uint32_t>& ids, std::vector<Polygon>& loops) {
    loops.clear();
    std::size_t cursor = 0;
    for (const WorkPoly& poly : polys) {
        Polygon loop;
        loop.face = static_cast<std::uint32_t>(poly.face);
        loop.vertices.assign(ids.begin() + static_cast<std::ptrdiff_t>(cursor),
                             ids.begin() + static_cast<std::ptrdiff_t>(cursor + poly.points.size()));
        cursor += poly.points.size();
        tidy(loop.vertices);
        loops.push_back(std::move(loop));
    }
}

Solid finish(const std::vector<WorkPoly>& polys, const std::vector<Plane>& planes, const Tolerances& tol) {
    std::vector<DVec3> points;
    for (const WorkPoly& poly : polys) points.insert(points.end(), poly.points.begin(), poly.points.end());
    std::vector<std::uint32_t> ids;
    Solid solid;
    weld(points, tol.weld, ids, solid.vertices);
    make_loops(polys, ids, solid.loops);
    prune(solid.loops, solid.vertices.size());
    refine(solid.vertices, solid.loops, planes, tol.refine);

    // Refining can bring two vertices together: weld once more.
    std::vector<DVec3> merged;
    weld(solid.vertices, tol.weld, ids, merged);
    for (Polygon& loop : solid.loops) {
        for (std::uint32_t& v : loop.vertices) v = ids[v];
        tidy(loop.vertices);
    }
    prune(solid.loops, merged.size());

    // Keep only the vertices still in use, in first-use order.
    std::vector<std::uint32_t> remap(merged.size(), kNone);
    solid.vertices.clear();
    for (Polygon& loop : solid.loops) {
        for (std::uint32_t& v : loop.vertices) {
            if (remap[v] == kNone) {
                remap[v] = static_cast<std::uint32_t>(solid.vertices.size());
                solid.vertices.push_back(merged[v]);
            }
            v = remap[v];
        }
    }
    return solid;
}

// Volume and centroid by the divergence theorem: a tetrahedron from a
// reference point to each fan triangle.
void mass_of(const std::vector<DVec3>& vertices, const std::vector<Polygon>& loops, double& volume_out,
             DVec3& centroid_out) {
    if (vertices.empty()) {
        volume_out = 0.0;
        centroid_out = {};
        return;
    }
    const DVec3 ref = vertices[0];
    double six_volume = 0.0;
    DVec3 moment;
    for (const Polygon& loop : loops) {
        const DVec3 a = vertices[loop.vertices[0]] - ref;
        for (std::size_t k = 1; k + 1 < loop.vertices.size(); ++k) {
            const DVec3 b = vertices[loop.vertices[k]] - ref;
            const DVec3 c = vertices[loop.vertices[k + 1]] - ref;
            const double t = dot(a, cross(b, c));
            six_volume += t;
            moment = moment + (a + b + c) * t;
        }
    }
    volume_out = six_volume / 6.0;
    centroid_out = six_volume != 0.0 ? ref + moment / (4.0 * six_volume) : ref;
}

void bounds_of(const std::vector<DVec3>& vertices, DVec3& lo, DVec3& hi) {
    if (vertices.empty()) {
        lo = hi = {};
        return;
    }
    lo = hi = vertices[0];
    for (const DVec3& v : vertices) {
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
}

// A start-box side: four corners, turned to face away from the centre.
WorkPoly box_side(int face, DVec3 a, DVec3 b, DVec3 c, DVec3 d, DVec3 centre) {
    WorkPoly poly;
    poly.face = face;
    poly.points = {a, b, c, d};
    if (dot(cross(b - a, c - a), a - centre) < 0.0) std::reverse(poly.points.begin(), poly.points.end());
    return poly;
}

}  // namespace

std::optional<Plane> plane_of(const Face& face) {
    const DVec3 e1 = face.p2 - face.p1;
    const DVec3 e2 = face.p3 - face.p1;
    const DVec3 c = cross(e1, e2);
    const double cl = length(c);
    // sin of the angle between the edges; tiny means a line (or a point).
    if (!(cl > 1e-10 * length(e1) * length(e2)) || cl == 0.0) return std::nullopt;
    Plane plane;
    plane.normal = c / cl;
    plane.distance = dot(plane.normal, face.p1);
    return plane;
}

void default_axes(DVec3 normal, DVec3& u_axis, DVec3& v_axis) {
    const DVec3 n = normalize(normal);
    const bool floor = std::fabs(n.y) >= std::fabs(n.x) && std::fabs(n.y) >= std::fabs(n.z);
    const DVec3 v = floor ? DVec3{0, 0, 1} : DVec3{0, -1, 0};
    v_axis = normalize(v - n * dot(n, v));
    u_axis = normalize(cross(n, v_axis));
}

Face face_through(DVec3 p1, DVec3 p2, DVec3 p3) {
    Face face;
    face.p1 = p1;
    face.p2 = p2;
    face.p3 = p3;
    if (const auto plane = plane_of(face)) default_axes(plane->normal, face.u_axis, face.v_axis);
    return face;
}

Face face_from_plane(DVec3 normal, DVec3 point) {
    DVec3 u;
    DVec3 v;
    default_axes(normal, u, v);
    // V x U is the normal, so point, point + V, point + U run counter-clockwise.
    Face face;
    face.p1 = point;
    face.p2 = point + v;
    face.p3 = point + u;
    face.u_axis = u;
    face.v_axis = v;
    return face;
}

Built build(std::vector<Face> faces) {
    Built built;
    std::vector<Plane> planes;
    planes.reserve(faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto plane = plane_of(faces[i]);
        if (!plane) {
            built.error = "face " + std::to_string(i + 1) + "'s points lie on a line";
            return built;
        }
        planes.push_back(*plane);
    }
    if (faces.empty()) {
        built.error = "the faces do not close a solid";
        return built;
    }

    DVec3 lo = faces[0].p1;
    DVec3 hi = lo;
    for (const Face& f : faces) {
        for (const DVec3& p : {f.p1, f.p2, f.p3}) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    const DVec3 centre = (lo + hi) * 0.5;
    const double extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1.0});
    const Tolerances tol = tolerances_for(std::max({extent, max_norm(lo), max_norm(hi)}));

    // The start box, 1000 times the points' size; its sides are faces -1..-6.
    const double h = 1000.0 * extent;
    const DVec3 b0 = centre - DVec3{h, h, h};
    const DVec3 b1 = centre + DVec3{h, h, h};
    auto corner = [&](int x, int y, int z) { return DVec3{x ? b1.x : b0.x, y ? b1.y : b0.y, z ? b1.z : b0.z}; };
    std::vector<WorkPoly> polys;
    polys.reserve(faces.size() + 6);
    polys.push_back(box_side(-1, corner(0, 0, 0), corner(0, 1, 0), corner(0, 1, 1), corner(0, 0, 1), centre));
    polys.push_back(box_side(-2, corner(1, 0, 0), corner(1, 1, 0), corner(1, 1, 1), corner(1, 0, 1), centre));
    polys.push_back(box_side(-3, corner(0, 0, 0), corner(1, 0, 0), corner(1, 0, 1), corner(0, 0, 1), centre));
    polys.push_back(box_side(-4, corner(0, 1, 0), corner(1, 1, 0), corner(1, 1, 1), corner(0, 1, 1), centre));
    polys.push_back(box_side(-5, corner(0, 0, 0), corner(1, 0, 0), corner(1, 1, 0), corner(0, 1, 0), centre));
    polys.push_back(box_side(-6, corner(0, 0, 1), corner(1, 0, 1), corner(1, 1, 1), corner(0, 1, 1), centre));

    Clipper clipper;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (clip_solid(polys, planes[i], static_cast<int>(i), tol.clip, clipper) == Cut::kEverything) {
            built.error = "the faces leave nothing";
            return built;
        }
    }
    for (const WorkPoly& poly : polys) {
        if (poly.face < 0) {
            built.error = "the faces do not close a solid";
            return built;
        }
    }

    Solid solid = finish(polys, planes, tol);
    if (solid.loops.size() < 4) {
        built.error = "the faces leave nothing";
        return built;
    }

    // Kept faces in their original order; polygons follow them.
    std::sort(solid.loops.begin(), solid.loops.end(),
              [](const Polygon& l, const Polygon& r) { return l.face < r.face; });
    Shape& shape = built.shape;
    built.faces.reserve(solid.loops.size());
    shape.planes.reserve(solid.loops.size());
    for (Polygon& loop : solid.loops) {
        built.faces.push_back(std::move(faces[loop.face]));
        shape.planes.push_back(planes[loop.face]);
        loop.face = static_cast<std::uint32_t>(built.faces.size() - 1);
    }
    shape.vertices = std::move(solid.vertices);
    shape.polygons = std::move(solid.loops);
    for (const Polygon& loop : shape.polygons) {
        for (std::size_t k = 0; k < loop.vertices.size(); ++k) {
            const std::uint32_t a = loop.vertices[k];
            const std::uint32_t b = loop.vertices[(k + 1) % loop.vertices.size()];
            shape.edges.emplace_back(std::min(a, b), std::max(a, b));
        }
    }
    std::sort(shape.edges.begin(), shape.edges.end());
    shape.edges.erase(std::unique(shape.edges.begin(), shape.edges.end()), shape.edges.end());
    bounds_of(shape.vertices, shape.min, shape.max);
    return built;
}

Built move_face(const std::vector<Face>& faces, std::size_t index, double distance) {
    if (index >= faces.size()) {
        Built built;
        built.error = "face " + std::to_string(index + 1) + " does not exist";
        return built;
    }
    std::vector<Face> moved = faces;
    if (const auto plane = plane_of(moved[index])) {
        const DVec3 shift = plane->normal * distance;
        moved[index].p1 = moved[index].p1 + shift;
        moved[index].p2 = moved[index].p2 + shift;
        moved[index].p3 = moved[index].p3 + shift;
    }
    return build(std::move(moved));
}

Built expand(const std::vector<Face>& faces, double distance) {
    std::vector<Face> moved = faces;
    for (Face& face : moved) {
        if (const auto plane = plane_of(face)) {
            const DVec3 shift = plane->normal * distance;
            face.p1 = face.p1 + shift;
            face.p2 = face.p2 + shift;
            face.p3 = face.p3 + shift;
        }
    }
    return build(std::move(moved));
}

Built clip(const std::vector<Face>& faces, const Face& face) {
    std::vector<Face> clipped = faces;
    clipped.push_back(face);
    return build(std::move(clipped));
}

Built transform(const std::vector<Face>& faces, const double m[12]) {
    auto point = [&](DVec3 p) {
        return DVec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                     m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    };
    auto linear = [&](DVec3 p) {
        return DVec3{m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z,
                     m[8] * p.x + m[9] * p.y + m[10] * p.z};
    };
    const double det =
        m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    std::vector<Face> moved = faces;
    for (Face& face : moved) {
        face.p1 = point(face.p1);
        face.p2 = point(face.p2);
        face.p3 = point(face.p3);
        // A mirror turns the winding inside out; swapping two points turns it back.
        if (det < 0.0) std::swap(face.p2, face.p3);
        const auto plane = plane_of(face);
        if (!plane) continue;  // the build refuses it
        const DVec3 n = plane->normal;
        auto into_plane = [&](DVec3 a) { return normalize(a - n * dot(a, n)); };
        const DVec3 u = into_plane(linear(face.u_axis));
        const DVec3 v = into_plane(linear(face.v_axis));
        if (u == DVec3{} || v == DVec3{}) {
            default_axes(n, face.u_axis, face.v_axis);
        } else {
            face.u_axis = u;
            face.v_axis = v;
        }
    }
    return build(std::move(moved));
}

std::vector<Face> faces_from_hull(const std::vector<std::vector<DVec3>>& polygons) {
    DVec3 centre;
    std::size_t count = 0;
    for (const auto& list : polygons) {
        for (const DVec3& p : list) centre = centre + p;
        count += list.size();
    }
    if (count > 0) centre = centre / static_cast<double>(count);
    std::vector<Face> faces;
    faces.reserve(polygons.size());
    for (const auto& list : polygons) {
        if (list.size() < 3) continue;
        for (std::size_t k = 2; k < list.size(); ++k) {
            const auto plane = plane_of(list[0], list[1], list[k]);
            if (!plane) continue;
            if (dot(plane->normal, centre) > plane->distance) {
                faces.push_back(face_through(list[0], list[k], list[1]));
            } else {
                faces.push_back(face_through(list[0], list[1], list[k]));
            }
            break;
        }
    }
    return faces;
}

std::vector<Face> make_box(DVec3 size) {
    const DVec3 h = size * 0.5;
    auto c = [&](int x, int y, int z) { return DVec3{x ? h.x : -h.x, y ? h.y : -h.y, z ? h.z : -h.z}; };
    return faces_from_hull({
        {c(0, 0, 0), c(0, 1, 0), c(0, 1, 1), c(0, 0, 1)},
        {c(1, 0, 0), c(1, 1, 0), c(1, 1, 1), c(1, 0, 1)},
        {c(0, 0, 0), c(1, 0, 0), c(1, 0, 1), c(0, 0, 1)},
        {c(0, 1, 0), c(1, 1, 0), c(1, 1, 1), c(0, 1, 1)},
        {c(0, 0, 0), c(1, 0, 0), c(1, 1, 0), c(0, 1, 0)},
        {c(0, 0, 1), c(1, 0, 1), c(1, 1, 1), c(0, 1, 1)},
    });
}

namespace {

// A regular polygon in XZ stretched to fill size.x by size.z exactly (an odd
// one is not symmetric, so it is fitted to its own extents).
std::vector<DVec3> ring(DVec3 size, int sides, double y) {
    sides = std::max(sides, 3);
    std::vector<DVec3> points(static_cast<std::size_t>(sides));
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (int i = 0; i < sides; ++i) {
        const double a = 2.0 * kPi * (i + 0.5) / sides;
        points[static_cast<std::size_t>(i)] = {std::cos(a), y, std::sin(a)};
        x0 = std::min(x0, std::cos(a));
        x1 = std::max(x1, std::cos(a));
        z0 = std::min(z0, std::sin(a));
        z1 = std::max(z1, std::sin(a));
    }
    for (DVec3& p : points) {
        p.x = -size.x * 0.5 + (p.x - x0) / (x1 - x0) * size.x;
        p.z = -size.z * 0.5 + (p.z - z0) / (z1 - z0) * size.z;
    }
    return points;
}

}  // namespace

std::vector<Face> make_cylinder(DVec3 size, int sides) {
    const std::vector<DVec3> bottom = ring(size, sides, -size.y * 0.5);
    const std::vector<DVec3> top = ring(size, sides, size.y * 0.5);
    std::vector<std::vector<DVec3>> polygons;
    for (std::size_t i = 0; i < bottom.size(); ++i) {
        const std::size_t j = (i + 1) % bottom.size();
        polygons.push_back({bottom[i], bottom[j], top[j], top[i]});
    }
    polygons.push_back(top);
    polygons.push_back(bottom);
    return faces_from_hull(polygons);
}

std::vector<Face> make_cone(DVec3 size, int sides) {
    const std::vector<DVec3> bottom = ring(size, sides, -size.y * 0.5);
    const DVec3 apex{0.0, size.y * 0.5, 0.0};
    std::vector<std::vector<DVec3>> polygons;
    for (std::size_t i = 0; i < bottom.size(); ++i) {
        polygons.push_back({bottom[i], bottom[(i + 1) % bottom.size()], apex});
    }
    polygons.push_back(bottom);
    return faces_from_hull(polygons);
}

std::vector<Face> make_sphere(DVec3 size, int detail) {
    detail = std::clamp(detail, 0, 4);
    const double t = (1.0 + std::sqrt(5.0)) * 0.5;
    const DVec3 v[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                         {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    const int f[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                          {11, 10, 2}, {10, 7, 6}, {7, 1, 8},   {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                          {3, 8, 9},  {4, 9, 5},  {2, 4, 11},  {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    std::vector<std::vector<DVec3>> tris;
    for (const auto& tri : f) tris.push_back({normalize(v[tri[0]]), normalize(v[tri[1]]), normalize(v[tri[2]])});
    for (int level = 0; level < detail; ++level) {
        std::vector<std::vector<DVec3>> next;
        next.reserve(tris.size() * 4);
        for (const auto& tri : tris) {
            // a + b is the same bits as b + a, so neighbours share midpoints exactly.
            const DVec3 ab = normalize(tri[0] + tri[1]);
            const DVec3 bc = normalize(tri[1] + tri[2]);
            const DVec3 ca = normalize(tri[2] + tri[0]);
            next.push_back({tri[0], ab, ca});
            next.push_back({ab, tri[1], bc});
            next.push_back({ca, bc, tri[2]});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }
    DVec3 lo{1e300, 1e300, 1e300};
    DVec3 hi = -lo;
    for (const auto& tri : tris) {
        for (const DVec3& p : tri) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    for (auto& tri : tris) {
        for (DVec3& p : tri) {
            p = {-size.x * 0.5 + (p.x - lo.x) / (hi.x - lo.x) * size.x,
                 -size.y * 0.5 + (p.y - lo.y) / (hi.y - lo.y) * size.y,
                 -size.z * 0.5 + (p.z - lo.z) / (hi.z - lo.z) * size.z};
        }
    }
    return faces_from_hull(tris);
}

bool contains_point(const Shape& shape, DVec3 point, double tolerance) {
    if (shape.planes.empty()) return false;
    for (const Plane& plane : shape.planes) {
        if (dot(plane.normal, point) - plane.distance > tolerance) return false;
    }
    return true;
}

double volume(const Shape& shape) {
    double v = 0.0;
    DVec3 c;
    mass_of(shape.vertices, shape.polygons, v, c);
    return v;
}

DVec3 centroid(const Shape& shape) {
    double v = 0.0;
    DVec3 c;
    mass_of(shape.vertices, shape.polygons, v, c);
    return c;
}

namespace {

void add_piece(std::vector<Piece>& out, const std::vector<DVec3>& vertices, const std::vector<Polygon>& loops) {
    Piece piece;
    piece.points = vertices;
    mass_of(vertices, loops, piece.volume, piece.centroid);
    out.push_back(std::move(piece));
}

void split(std::vector<WorkPoly> polys, std::vector<Plane>& planes, const Tolerances& tol, std::size_t max_vertices,
           std::size_t max_faces, int depth, Clipper& clipper, std::vector<Piece>& out) {
    const Solid solid = finish(polys, planes, tol);
    if (solid.loops.size() < 4) return;  // a sliver with no volume
    if ((solid.vertices.size() <= max_vertices && solid.loops.size() <= max_faces) || depth >= 32) {
        add_piece(out, solid.vertices, solid.loops);
        return;
    }
    // Through the centre, across the longest side.
    DVec3 lo;
    DVec3 hi;
    bounds_of(solid.vertices, lo, hi);
    const DVec3 extent = hi - lo;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    double v = 0.0;
    DVec3 c;
    mass_of(solid.vertices, solid.loops, v, c);
    Plane below;
    below.normal = {axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0};
    below.distance = get(c, axis);
    const Plane above{-below.normal, -below.distance};
    const int below_face = static_cast<int>(planes.size());
    planes.push_back(below);
    planes.push_back(above);

    std::vector<WorkPoly> other = polys;
    const Cut a = clip_solid(polys, below, below_face, tol.clip, clipper);
    const Cut b = clip_solid(other, above, below_face + 1, tol.clip, clipper);
    if (a != Cut::kSome || b != Cut::kSome) {
        add_piece(out, solid.vertices, solid.loops);  // cannot cut further
        return;
    }
    split(std::move(polys), planes, tol, max_vertices, max_faces, depth + 1, clipper, out);
    split(std::move(other), planes, tol, max_vertices, max_faces, depth + 1, clipper, out);
}

}  // namespace

std::vector<Piece> hull_pieces(const Shape& shape, std::size_t max_vertices, std::size_t max_faces) {
    std::vector<Piece> out;
    if (shape.polygons.empty()) return out;
    if (shape.vertices.size() <= max_vertices && shape.polygons.size() <= max_faces) {
        add_piece(out, shape.vertices, shape.polygons);
        return out;
    }
    std::vector<WorkPoly> polys;
    polys.reserve(shape.polygons.size());
    for (const Polygon& loop : shape.polygons) {
        WorkPoly poly;
        poly.face = static_cast<int>(loop.face);
        for (std::uint32_t v : loop.vertices) poly.points.push_back(shape.vertices[v]);
        polys.push_back(std::move(poly));
    }
    std::vector<Plane> planes = shape.planes;
    const Tolerances tol = tolerances_for(std::max(max_norm(shape.min), max_norm(shape.max)));
    Clipper clipper;
    split(std::move(polys), planes, tol, max_vertices, max_faces, 0, clipper, out);
    return out;
}

Mesh build_mesh(const std::vector<Face>& faces, const Shape& shape) {
    Mesh mesh;
    // Each polygon's range: one per Material, in order of first use.
    std::vector<std::uint32_t> range_of(shape.polygons.size());
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
    for (std::size_t i = 0; i < shape.polygons.size(); ++i) {
        const std::string& material = faces[shape.polygons[i].face].material;
        std::size_t r = 0;
        while (r < mesh.ranges.size() && mesh.ranges[r].material != material) ++r;
        if (r == mesh.ranges.size()) {
            MeshRange range;
            range.material = material;
            mesh.ranges.push_back(std::move(range));
        }
        range_of[i] = static_cast<std::uint32_t>(r);
        vertex_count += shape.polygons[i].vertices.size();
        index_count += 3 * (shape.polygons[i].vertices.size() - 2);
    }
    std::vector<std::uint32_t> order(shape.polygons.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t l, std::uint32_t r) { return range_of[l] < range_of[r]; });
    mesh.vertices.reserve(vertex_count);
    mesh.indices.reserve(index_count);

    auto grow = [](float* lo, float* hi, const float* p, bool first) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = first ? p[k] : std::min(lo[k], p[k]);
            hi[k] = first ? p[k] : std::max(hi[k], p[k]);
        }
    };

    for (std::uint32_t pi : order) {
        const Polygon& poly = shape.polygons[pi];
        const Face& face = faces[poly.face];
        const DVec3 n = shape.planes[poly.face].normal;
        DVec3 u = face.u_axis;
        DVec3 v = face.v_axis;
        if (length(u) < 1e-12 || length(v) < 1e-12) default_axes(n, u, v);
        // Rotation turns both axes about the normal (Rodrigues).
        if (face.rotation != 0.0) {
            const double a = face.rotation * kPi / 180.0;
            const double cs = std::cos(a);
            const double sn = std::sin(a);
            auto turn = [&](DVec3 x) { return x * cs + cross(n, x) * sn + n * (dot(n, x) * (1.0 - cs)); };
            u = turn(u);
            v = turn(v);
        }
        const double su = face.scale_u != 0.0 ? face.scale_u : 1.0;
        const double sv = face.scale_v != 0.0 ? face.scale_v : 1.0;
        const DVec3 tangent = normalize(u);
        const float w = dot(cross(n, u), v) < 0.0 ? -1.f : 1.f;

        MeshRange& range = mesh.ranges[range_of[pi]];
        const bool range_first = range.index_count == 0;
        if (range_first) range.first_index = static_cast<std::uint32_t>(mesh.indices.size());
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (std::size_t k = 0; k < poly.vertices.size(); ++k) {
            const DVec3 p = shape.vertices[poly.vertices[k]];
            MeshVertex mv;
            mv.position[0] = static_cast<float>(p.x);
            mv.position[1] = static_cast<float>(p.y);
            mv.position[2] = static_cast<float>(p.z);
            mv.normal[0] = static_cast<float>(n.x);
            mv.normal[1] = static_cast<float>(n.y);
            mv.normal[2] = static_cast<float>(n.z);
            mv.uv[0] = static_cast<float>(dot(p, u) / su + face.offset_u);
            mv.uv[1] = static_cast<float>(dot(p, v) / sv + face.offset_v);
            mv.tangent[0] = static_cast<float>(tangent.x);
            mv.tangent[1] = static_cast<float>(tangent.y);
            mv.tangent[2] = static_cast<float>(tangent.z);
            mv.tangent[3] = w;
            grow(range.min, range.max, mv.position, range_first && k == 0);
            grow(mesh.min, mesh.max, mv.position, mesh.vertices.empty());
            mesh.vertices.push_back(mv);
        }
        for (std::uint32_t k = 1; k + 1 < poly.vertices.size(); ++k) {
            mesh.indices.push_back(base);
            mesh.indices.push_back(base + k);
            mesh.indices.push_back(base + k + 1);
        }
        range.index_count += static_cast<std::uint32_t>(3 * (poly.vertices.size() - 2));
    }
    return mesh;
}

std::optional<std::size_t> face_at(const Shape& shape, DVec3 point, DVec3 normal, double tolerance) {
    std::optional<std::size_t> best;
    double best_dot = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < shape.planes.size(); ++i) {
        const Plane& plane = shape.planes[i];
        if (std::fabs(dot(plane.normal, point) - plane.distance) > tolerance) continue;
        const double d = dot(plane.normal, normal);
        if (d > best_dot) {
            best_dot = d;
            best = i;
        }
    }
    return best;
}

}  // namespace engine_core::brush

namespace engine_core::brush {

// Every plane through three of the points with all the others behind it, so
// the faces of their convex hull; a brute force fit for the few dozen corners
// a hand-edited brush has. Coplanar duplicates are dropped by build().
std::optional<std::vector<Face>> hull_faces(const std::vector<DVec3>& points) {
    const std::size_t n = points.size();
    if (n < 4) {
        return std::nullopt;
    }
    DVec3 lo = points[0];
    DVec3 hi = points[0];
    for (const DVec3& p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const double eps = 1e-7 * std::max(1.0, length(hi - lo));
    std::vector<Face> faces;
    std::vector<Plane> seen;
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = a + 1; b < n; ++b) {
            for (std::size_t c = b + 1; c < n; ++c) {
                DVec3 normal = cross(points[b] - points[a], points[c] - points[a]);
                const double len = length(normal);
                if (len <= eps * eps) {
                    continue;
                }
                normal = normal / len;
                double d = dot(normal, points[a]);
                int above = 0;
                int below = 0;
                for (const DVec3& p : points) {
                    const double side = dot(normal, p) - d;
                    above += side > eps ? 1 : 0;
                    below += side < -eps ? 1 : 0;
                }
                if (above != 0 && below != 0) {
                    continue;
                }
                if (above != 0) {
                    normal = -normal;
                    d = -d;
                }
                bool duplicate = false;
                for (const Plane& plane : seen) {
                    if (dot(plane.normal, normal) > 1.0 - 1e-9 && std::fabs(plane.distance - d) <= eps) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }
                seen.push_back(Plane{normal, d});
                faces.push_back(face_from_plane(normal, normal * d));
            }
        }
    }
    if (faces.size() < 4) {
        return std::nullopt;
    }
    return faces;
}

}  // namespace engine_core::brush
