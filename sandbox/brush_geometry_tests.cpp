// The geometry under Brush: faces in, a convex solid, hull pieces, and a mesh
// out. No instances; nothing here touches the DataModel.

#include "brush/BrushGeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace engine_core::brush;

namespace {

bool near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }
bool near(DVec3 a, DVec3 b, double tolerance = 1e-9) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// The index of the face whose plane has this outward normal.
std::size_t face_facing(const Shape& shape, DVec3 normal) {
    for (std::size_t i = 0; i < shape.planes.size(); ++i) {
        if (near(shape.planes[i].normal, normal, 1e-9)) return i;
    }
    return shape.planes.size();
}

}  // namespace

TEST_CASE("BG1 a box builds with exact vertices", "[brush]") {
    const Built built = build(make_box({4, 4, 4}));
    REQUIRE(built.ok());
    REQUIRE(built.faces.size() == 6);
    REQUIRE(built.shape.vertices.size() == 8);
    REQUIRE(built.shape.polygons.size() == 6);
    REQUIRE(built.shape.edges.size() == 12);
    for (const DVec3& v : built.shape.vertices) {
        REQUIRE(std::fabs(v.x) == 2.0);
        REQUIRE(std::fabs(v.y) == 2.0);
        REQUIRE(std::fabs(v.z) == 2.0);
    }
    for (std::size_t i = 0; i < built.shape.polygons.size(); ++i) {
        REQUIRE(built.shape.polygons[i].face == i);
        REQUIRE(built.shape.polygons[i].vertices.size() == 4);
    }
    REQUIRE(built.shape.min == DVec3{-2, -2, -2});
    REQUIRE(built.shape.max == DVec3{2, 2, 2});
    REQUIRE(near(volume(built.shape), 64.0));
    REQUIRE(near(centroid(built.shape), DVec3{}));
}

TEST_CASE("BG2 refusals say why", "[brush]") {
    std::vector<Face> faces = make_box({4, 4, 4});
    std::vector<Face> line = faces;
    line[2].p3 = line[2].p1 + (line[2].p2 - line[2].p1) * 3.0;
    REQUIRE(build(line).error == "face 3's points lie on a line");

    std::vector<Face> open(faces.begin(), faces.end() - 1);
    REQUIRE(build(open).error == "the faces do not close a solid");
    REQUIRE(build({}).error == "the faces do not close a solid");

    // A face turned inside out leaves nothing on the inner side of all six.
    std::vector<Face> nothing = faces;
    nothing.push_back(face_from_plane({-1, 0, 0}, {3, 0, 0}));
    REQUIRE(build(nothing).error == "the faces leave nothing");
}

TEST_CASE("BG3 a face that cuts nothing is dropped", "[brush]") {
    std::vector<Face> faces = make_box({4, 4, 4});
    faces.insert(faces.begin() + 2, face_from_plane({1, 1, 1}, {5, 5, 5}));
    const Built built = build(faces);
    REQUIRE(built.ok());
    REQUIRE(built.faces.size() == 6);
    REQUIRE(built.faces == make_box({4, 4, 4}));
}

TEST_CASE("BG4 a diagonal clip cuts a corner off", "[brush]") {
    const Built built = clip(make_box({4, 4, 4}), face_from_plane({1, 1, 0}, {0, 0, 0}));
    REQUIRE(built.ok());
    REQUIRE(built.faces.size() == 5);  // +X and +Y are gone entirely
    REQUIRE(built.shape.vertices.size() == 6);
    REQUIRE(near(volume(built.shape), 32.0));
    REQUIRE(!contains_point(built.shape, {1.5, 1.5, 0}));
    REQUIRE(contains_point(built.shape, {-1.5, -1.5, 0}));

    const Built corner = clip(make_box({4, 4, 4}), face_from_plane({1, 1, 1}, {1, 1, 1}));
    REQUIRE(corner.ok());
    REQUIRE(corner.faces.size() == 7);
    REQUIRE(corner.shape.vertices.size() == 10);
    REQUIRE(near(volume(corner.shape), 64.0 - 4.5));
}

TEST_CASE("BG14 a cut with no Material wears the face it most nearly faces", "[brush]") {
    std::vector<Face> box = make_box({4, 4, 4});
    for (Face& face : box) {
        const auto plane = plane_of(face);
        face.material = plane->normal.y > 0.5 ? "top" : "side";
        face.scale_u = plane->normal.y > 0.5 ? 2.0 : 1.0;
    }
    const Built slope = clip(box, face_from_plane({0, 1, 0.5}, {0, 1, 0}));
    REQUIRE(slope.ok());
    REQUIRE(slope.faces.back().material == "top");
    REQUIRE(slope.faces.back().scale_u == 2.0);

    // A Material given is kept.
    Face painted = face_from_plane({1, 0, 0.2}, {1, 0, 0});
    painted.material = "mine";
    REQUIRE(clip(box, painted).faces.back().material == "mine");
    // A side-on cut takes a side's.
    REQUIRE(clip(box, face_from_plane({1, 0.2, 0}, {1, 0, 0})).faces.back().material == "side");
}

TEST_CASE("BG5 move_face and expand", "[brush]") {
    const std::vector<Face> box = make_box({4, 4, 4});
    const std::size_t top = face_facing(build(box).shape, {0, 1, 0});
    const Built moved = move_face(box, top, 1.0);
    REQUIRE(moved.ok());
    REQUIRE(moved.shape.max == DVec3{2, 3, 2});
    REQUIRE(moved.shape.min == DVec3{-2, -2, -2});

    REQUIRE(move_face(box, top, -4.0).error == "the faces leave nothing");
    REQUIRE(move_face(box, top, -5.0).error == "the faces leave nothing");
    REQUIRE(!move_face(box, 6, 1.0).ok());

    const Built grown = expand(box, 1.0);
    REQUIRE(grown.ok());
    REQUIRE(grown.shape.max == DVec3{3, 3, 3});
    const Built shrunk = expand(box, -1.5);
    REQUIRE(shrunk.ok());
    REQUIRE(shrunk.shape.max == DVec3{0.5, 0.5, 0.5});
    REQUIRE(expand(box, -2.0).error == "the faces leave nothing");
}

TEST_CASE("BG6 the shape makers fill their size", "[brush]") {
    const DVec3 size{3, 5, 7};
    struct Case {
        std::vector<Face> faces;
        std::size_t count;
    };
    const Case cases[] = {
        {make_box(size), 6},
        {make_cylinder(size, 3), 5},
        {make_cylinder(size, 12), 14},
        {make_cone(size, 5), 6},
        {make_cone(size, 16), 17},
        {make_sphere(size, 0), 20},
        {make_sphere(size, 1), 80},
        {make_sphere(size, 2), 320},
    };
    for (const Case& c : cases) {
        REQUIRE(c.faces.size() == c.count);
        const Built built = build(c.faces);
        REQUIRE(built.ok());
        REQUIRE(built.faces.size() == c.count);
        REQUIRE(near(built.shape.min, size * -0.5, 1e-9));
        REQUIRE(near(built.shape.max, size * 0.5, 1e-9));
    }
    // Fewer than three sides is three.
    REQUIRE(make_cylinder(size, 1).size() == 5);
}

TEST_CASE("BG7 transform moves, turns, and mirrors", "[brush]") {
    const std::vector<Face> box = make_box({4, 2, 2});
    const double shift[12] = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0};
    const Built moved = transform(box, shift);
    REQUIRE(moved.ok());
    REQUIRE(moved.shape.min == DVec3{8, -1, -1});

    // Turn 90 degrees about Y: x goes to -z.
    const double turn[12] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    const Built turned = transform(box, turn);
    REQUIRE(turned.ok());
    REQUIRE(near(turned.shape.max, DVec3{1, 1, 2}));

    const double mirror[12] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const Built mirrored = transform(make_cone({2, 2, 2}, 5), mirror);
    REQUIRE(mirrored.ok());
    REQUIRE(mirrored.faces.size() == 6);
    REQUIRE(volume(mirrored.shape) > 0.0);
    for (const Face& f : mirrored.faces) {
        const DVec3 n = plane_of(f)->normal;
        REQUIRE(near(dot(f.u_axis, n), 0.0));
        REQUIRE(near(length(f.u_axis), 1.0));
    }
}

TEST_CASE("BG8 a big brush splits into hull-sized pieces", "[brush]") {
    const Built built = build(make_sphere({10, 6, 8}, 3));
    REQUIRE(built.ok());
    REQUIRE(built.faces.size() == 1280);
    const std::vector<Piece> pieces = hull_pieces(built.shape, 128, 128);
    REQUIRE(pieces.size() > 1);
    double total = 0.0;
    for (const Piece& piece : pieces) {
        REQUIRE(piece.points.size() <= 128);
        REQUIRE(piece.volume > 0.0);
        total += piece.volume;
    }
    const double whole = volume(built.shape);
    REQUIRE(std::fabs(total - whole) <= 1e-6 * whole);

    // A small one stays whole.
    const Built box = build(make_box({1, 1, 1}));
    const std::vector<Piece> one = hull_pieces(box.shape, 128, 128);
    REQUIRE(one.size() == 1);
    REQUIRE(near(one[0].volume, 1.0));
}

TEST_CASE("BG9 UVs follow axes, offset, scale, rotation", "[brush]") {
    std::vector<Face> faces = make_box({4, 4, 4});
    const Built built = build(faces);
    const std::size_t front = face_facing(built.shape, {0, 0, 1});
    REQUIRE(built.faces[front].u_axis == DVec3{1, 0, 0});
    REQUIRE(built.faces[front].v_axis == DVec3{0, -1, 0});
    const std::size_t top = face_facing(built.shape, {0, 1, 0});
    REQUIRE(built.faces[top].v_axis == DVec3{0, 0, 1});

    // The UV at the front face's corner (2, -2, 2).
    auto uv_at = [&](const std::vector<Face>& fs, float out[2], float tangent[4]) {
        const Built b = build(fs);
        const Mesh mesh = build_mesh(b.faces, b.shape);
        for (const MeshVertex& v : mesh.vertices) {
            if (v.normal[2] == 1.f && v.position[0] == 2.f && v.position[1] == -2.f) {
                out[0] = v.uv[0];
                out[1] = v.uv[1];
                for (int k = 0; k < 4; ++k) tangent[k] = v.tangent[k];
            }
        }
    };
    float uv[2] = {99, 99};
    float tangent[4] = {};
    uv_at(built.faces, uv, tangent);
    REQUIRE(uv[0] == 2.f);
    REQUIRE(uv[1] == 2.f);
    REQUIRE(tangent[0] == 1.f);
    // V runs down the wall: cross(n, U) is +Y, so the bitangent's sign is -1.
    REQUIRE(tangent[3] == -1.f);

    std::vector<Face> edited = built.faces;
    edited[front].offset_u = 0.5;
    edited[front].scale_v = 4.0;
    uv_at(edited, uv, tangent);
    REQUIRE(uv[0] == 2.5f);
    REQUIRE(uv[1] == 0.5f);

    edited = built.faces;
    edited[front].rotation = 90.0;  // U turns to +Y, V to +X
    uv_at(edited, uv, tangent);
    REQUIRE(near(uv[0], -2.0, 1e-6));
    REQUIRE(near(uv[1], 2.0, 1e-6));
    REQUIRE(near(tangent[1], 1.0, 1e-6));
    REQUIRE(tangent[3] == -1.f);

    // A mirrored V flips the sign.
    edited = built.faces;
    edited[front].v_axis = DVec3{0, 1, 0};
    uv_at(edited, uv, tangent);
    REQUIRE(tangent[3] == 1.f);
}

TEST_CASE("BG10 the mesh groups triangles by Material", "[brush]") {
    std::vector<Face> faces = make_box({4, 4, 4});
    faces[1].material = "stone";
    faces[4].material = "stone";
    faces[3].material = "wood";
    const Built built = build(faces);
    const Mesh mesh = build_mesh(built.faces, built.shape);
    REQUIRE(mesh.vertices.size() == 24);
    REQUIRE(mesh.indices.size() == 36);
    REQUIRE(mesh.ranges.size() == 3);
    REQUIRE(mesh.ranges[0].material.empty());
    REQUIRE(mesh.ranges[1].material == "stone");
    REQUIRE(mesh.ranges[2].material == "wood");
    REQUIRE(mesh.ranges[0].index_count == 18);
    REQUIRE(mesh.ranges[1].index_count == 12);
    REQUIRE(mesh.ranges[1].first_index == 18);
    REQUIRE(mesh.ranges[2].first_index == 30);
    REQUIRE(mesh.min[0] == -2.f);
    REQUIRE(mesh.max[1] == 2.f);
    // The wood face is one side, so its bounds are flat on one axis.
    const MeshRange& wood = mesh.ranges[2];
    int flat = 0;
    for (int k = 0; k < 3; ++k) flat += wood.min[k] == wood.max[k];
    REQUIRE(flat == 1);
}

TEST_CASE("BG11 face_at and contains_point", "[brush]") {
    const Built built = build(make_box({4, 4, 4}));
    const Shape& shape = built.shape;
    const std::size_t top = face_facing(shape, {0, 1, 0});
    REQUIRE(face_at(shape, {0.5, 2.00001, 0.5}, {0, 1, 0}) == top);
    // On an edge, the normal decides.
    const std::size_t right = face_facing(shape, {1, 0, 0});
    REQUIRE(face_at(shape, {2, 2, 0}, {0.9, 0.1, 0}) == right);
    REQUIRE(face_at(shape, {2, 2, 0}, {0.1, 0.9, 0}) == top);
    REQUIRE(!face_at(shape, {0, 0, 0}, {0, 1, 0}));

    REQUIRE(contains_point(shape, {0, 0, 0}));
    REQUIRE(contains_point(shape, {2, 2, 2}));
    REQUIRE(!contains_point(shape, {2.1, 0, 0}));
}

TEST_CASE("BG12 builds are quick", "[brush]") {
    const std::vector<Face> box = make_box({4, 4, 4});
    const std::vector<Face> sphere3 = make_sphere({8, 8, 8}, 3);
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    auto t0 = clock::now();
    for (int i = 0; i < 1000; ++i) REQUIRE(build(box).ok());
    auto t1 = clock::now();
    const Built b3 = build(sphere3);
    auto t2 = clock::now();
    REQUIRE(b3.faces.size() == 1280);
    std::printf("brush builds: 1000 boxes %.1f ms, sphere 1280 faces %.1f ms\n", ms(t1 - t0), ms(t2 - t1));
    REQUIRE(ms(t2 - t1) < 5000.0);
}

TEST_CASE("BG13 a moved corner rebuilds as the hull of the corners", "[brush]") {
    const Built box = build(make_box({4, 4, 4}));
    REQUIRE(box.ok());
    std::vector<DVec3> points = box.shape.vertices;
    bool moved = false;
    for (DVec3& p : points) {
        if (p.x == 2 && p.y == 2 && p.z == 2) {
            p = {1, 3, 2};
            moved = true;
        }
    }
    REQUIRE(moved);
    const auto faces = hull_faces(points);
    REQUIRE(faces);
    const Built built = build(*faces);
    INFO(built.error);
    REQUIRE(built.ok());
    REQUIRE(built.shape.vertices.size() == 8);
}
