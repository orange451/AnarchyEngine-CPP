// ConvexDecomposition: V-HACD's pieces of a mesh, the memory cache, and the
// studio's queue that writes pieces into a Mesh's file.

#include "support.hpp"

#include "ConvexDecomposition.hpp"
#include "MeshShapes.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace {

using engine_core::Vec3;

struct Geometry {
    std::vector<Vec3> points;
    std::vector<std::uint32_t> triangles;
};

Geometry geometry_of(const anarchy::amesh::Data& data) {
    Geometry out;
    for (const auto& v : data.vertices) {
        out.points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    out.triangles = data.indices;
    return out;
}

// An L: a bar along X from 0 to 3, one high, and a post on its left end up to 3.
Geometry ell() {
    anarchy::amesh::Data data;
    engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
    engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{0.5f, 2.f, 0.f});
    return geometry_of(data);
}

}  // namespace

TEST_CASE("D1 an L splits into at least two convex pieces inside its bounds", "[decomposition]") {
    const Geometry l = ell();
    const std::vector<anarchy::amesh::ConvexPiece> pieces = engine_core::decompose(l.points, l.triangles);
    REQUIRE(pieces.size() >= 2);
    for (const auto& piece : pieces) {
        REQUIRE(piece.points.size() >= 4);
        REQUIRE(piece.points.size() <= 64);
        for (const auto& p : piece.points) {
            REQUIRE(p[0] >= -0.05f);
            REQUIRE(p[0] <= 3.05f);
            REQUIRE(p[1] >= -0.05f);
            REQUIRE(p[1] <= 3.05f);
            REQUIRE(p[2] >= -0.55f);
            REQUIRE(p[2] <= 0.55f);
        }
    }
}

TEST_CASE("D1b nothing to decompose gives no pieces", "[decomposition]") {
    REQUIRE(engine_core::decompose({}, {}).empty());
}
