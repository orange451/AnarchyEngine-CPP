#pragma once

// The geometry under a Brush: faces in, a convex solid out. No instances, no
// Lua, no GL; a pure function of its faces, callable from any thread.
//
// A face is three points on a plane, counter-clockwise seen from outside, as
// TrenchBroom stores one, plus its Material GUID and Valve 220 texture
// alignment. The solid is what is behind every face's plane: a box much larger
// than the points is clipped by each plane in turn (TrenchBroom's
// Brush::updateGeometryFromFaces). Doubles throughout; the mesh is floats.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine_core::brush {

struct DVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline DVec3 operator+(DVec3 a, DVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline DVec3 operator-(DVec3 a, DVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline DVec3 operator-(DVec3 a) { return {-a.x, -a.y, -a.z}; }
inline DVec3 operator*(DVec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline DVec3 operator*(double s, DVec3 a) { return a * s; }
inline DVec3 operator/(DVec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
inline bool operator==(DVec3 a, DVec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline bool operator!=(DVec3 a, DVec3 b) { return !(a == b); }
double dot(DVec3 a, DVec3 b);
DVec3 cross(DVec3 a, DVec3 b);
double length(DVec3 a);
// Zero stays zero.
DVec3 normalize(DVec3 a);

// The plane normal . x == distance; the solid is on the side where
// normal . x <= distance. The normal is a unit vector pointing outward.
struct Plane {
    DVec3 normal;
    double distance = 0.0;
};

struct Face {
    // Counter-clockwise seen from outside, in the Brush's local space.
    DVec3 p1;
    DVec3 p2;
    DVec3 p3;
    // The Material's GUID; empty is none (the default material).
    std::string material;
    // Texture axes in local space. Rotation turns them about the face normal.
    DVec3 u_axis;
    DVec3 v_axis;
    // Texture shift, in repeats.
    double offset_u = 0.0;
    double offset_v = 0.0;
    // Multiplies the Material's TextureScale (units per repeat).
    double scale_u = 1.0;
    double scale_v = 1.0;
    // Degrees.
    double rotation = 0.0;
};

bool operator==(const Face& a, const Face& b);
inline bool operator!=(const Face& a, const Face& b) { return !(a == b); }

// The plane through a face's points; nullopt when they lie on a line.
std::optional<Plane> plane_of(const Face& face);
inline std::optional<Plane> plane_of(DVec3 p1, DVec3 p2, DVec3 p3) {
    Face face;
    face.p1 = p1;
    face.p2 = p2;
    face.p3 = p3;
    return plane_of(face);
}

// The texture axes a face gets from the nearest world axis: V points down the
// wall (-Y) on walls and along +Z on floors and ceilings, U is normal x V, and
// both lie in the face, so slopes do not stretch.
void default_axes(DVec3 normal, DVec3& u_axis, DVec3& v_axis);

// A face through three points with default axes and alignment. The points must
// not be collinear (check with plane_of first).
Face face_through(DVec3 p1, DVec3 p2, DVec3 p3);
// A face from its outward normal and a point on it. The normal must not be zero.
Face face_from_plane(DVec3 normal, DVec3 point);

// The solid: one polygon per kept face, vertices shared.
struct Polygon {
    // Index into the brush's (kept) face list.
    std::uint32_t face = 0;
    // Into Shape::vertices, counter-clockwise seen from outside.
    std::vector<std::uint32_t> vertices;
};

struct Shape {
    std::vector<DVec3> vertices;
    // In face order: polygons[i].face == i.
    std::vector<Polygon> polygons;
    // planes[i] is face i's.
    std::vector<Plane> planes;
    // Each edge once, lower index first.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    DVec3 min;
    DVec3 max;
};

// A built brush: the faces that cut the solid, in their original order, and
// its shape; or why the faces were refused.
struct Built {
    std::vector<Face> faces;
    Shape shape;
    std::string error;
    bool ok() const { return error.empty(); }
};

// The build. Refused, with the reason, only when a face's points lie on a line
// ("face 3's points lie on a line"), when nothing is left ("the faces leave
// nothing"), or when the faces do not enclose a solid ("the faces do not close
// a solid"). Faces whose planes cut nothing are dropped from the result.
Built build(std::vector<Face> faces);

// Edits. Each builds the result, so a refusal leaves the caller's faces as they
// were. Face indices start at 0 here.
Built move_face(const std::vector<Face>& faces, std::size_t index, double distance);
Built expand(const std::vector<Face>& faces, double distance);
Built clip(const std::vector<Face>& faces, const Face& face);
// Every face's points moved by the matrix (row-major 3x4: rotation/scale then
// translation), axes turned with it. Mirroring matrices keep faces outward.
Built transform(const std::vector<Face>& faces, const double matrix[12]);

// Shapes centred on the origin, filling size exactly. No Materials.
std::vector<Face> make_box(DVec3 size);
std::vector<Face> make_cylinder(DVec3 size, int sides);
std::vector<Face> make_cone(DVec3 size, int sides);
std::vector<Face> make_sphere(DVec3 size, int detail);
// Faces from points, each list one convex polygon counter-clockwise seen from
// outside. A list's first three non-collinear points make the face.
std::vector<Face> faces_from_hull(const std::vector<DVec3>& points);

// Queries on a built shape.
bool contains_point(const Shape& shape, DVec3 point, double tolerance = 1e-6);
double volume(const Shape& shape);
DVec3 centroid(const Shape& shape);

// Convex pieces small enough for a physics hull: each at most max_vertices
// vertices and max_faces faces. A shape that fits is one piece; one that does
// not is cut through its centre across its longest side, and each half again.
// Halves of a convex solid are convex and fill it exactly.
struct Piece {
    std::vector<DVec3> points;
    double volume = 0.0;
    DVec3 centroid;
};
std::vector<Piece> hull_pieces(const Shape& shape, std::size_t max_vertices, std::size_t max_faces);

// The render mesh. UVs are in repeats before the Material's TextureScale:
// u = (p . U') / scale_u + offset_u, divided by TextureScale in the shader.
struct MeshVertex {
    float position[3];
    float normal[3];
    float uv[2];
    // Along U'; w is the bitangent's sign (bitangent = cross(normal, tangent) * w,
    // pointing along increasing v).
    float tangent[4];
};

struct MeshRange {
    // The Material GUID; empty is none.
    std::string material;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // Local bounds of this range's triangles.
    float min[3] = {0.f, 0.f, 0.f};
    float max[3] = {0.f, 0.f, 0.f};
};

struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    // One per Material, in order of first use.
    std::vector<MeshRange> ranges;
    float min[3] = {0.f, 0.f, 0.f};
    float max[3] = {0.f, 0.f, 0.f};
};

Mesh build_mesh(const std::vector<Face>& faces, const Shape& shape);

// The face whose plane holds a local point (within tolerance) with the normal
// nearest the given one; nullopt when none does.
std::optional<std::size_t> face_at(const Shape& shape, DVec3 point, DVec3 normal, double tolerance = 1e-4);

}  // namespace engine_core::brush
