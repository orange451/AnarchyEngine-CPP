// PoseBlend: a frame's last physics substep captured, and the render window's
// pose rewound through it.

#include "PoseBlend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using engine_core::Matrix4;
using engine_core::PoseBlend;

namespace {

bool near(const Matrix4& a, const Matrix4& b, float tolerance = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

Matrix4 turned(float x, float radians) {
    return engine_core::matrix4_multiply(at(x, 0, 0), engine_core::matrix4_axis_angle({0, 1, 0}, radians));
}

}  // namespace

TEST_CASE("PB1 records only while capturing, first before and last after", "[pose_blend]") {
    PoseBlend blend;
    blend.record(7, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    REQUIRE(blend.entries().empty());
    blend.begin_capture();
    blend.record(7, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    blend.record(7, PoseBlend::Kind::Body, at(1, 0, 0), at(2, 0, 0));
    blend.end_capture();
    REQUIRE(blend.entries().size() == 1);
    REQUIRE(near(blend.entries()[0].before, at(0, 0, 0)));
    REQUIRE(near(blend.entries()[0].after, at(2, 0, 0)));
    blend.begin_capture();
    REQUIRE(blend.entries().empty());
}

TEST_CASE("PB2 blends position and rotation through the substep", "[pose_blend]") {
    const PoseBlend::Entry moved{1, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0)};
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 0.0), at(0, 0, 0)));
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 0.25), at(0.25f, 0, 0)));
    REQUIRE(near(PoseBlend::blended(moved, at(1, 0, 0), 1.0), at(1, 0, 0)));
    const PoseBlend::Entry spun{2, PoseBlend::Kind::Body, turned(0, 0.f), turned(0, 0.4f)};
    REQUIRE(near(PoseBlend::blended(spun, turned(0, 0.4f), 0.5), turned(0, 0.2f)));
}

TEST_CASE("PB3 a pose written after the substep keeps its motion", "[pose_blend]") {
    // A script turned the character after physics moved it 1 unit along X.
    const PoseBlend::Entry moved{1, PoseBlend::Kind::Spatial, at(0, 0, 0), at(1, 0, 0)};
    const Matrix4 scripted = turned(1, 1.0f);
    const Matrix4 shown = PoseBlend::blended(moved, scripted, 0.5);
    REQUIRE(near(shown, turned(0.5f, 1.0f)));
}

TEST_CASE("PB4 clear forgets every entry", "[pose_blend]") {
    PoseBlend blend;
    blend.begin_capture();
    blend.record(3, PoseBlend::Kind::Body, at(0, 0, 0), at(1, 0, 0));
    blend.end_capture();
    blend.clear();
    REQUIRE(blend.entries().empty());
    REQUIRE_FALSE(blend.capturing());
}
