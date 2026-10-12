// PoseBlend: a frame's last physics substep captured, and the render window's
// pose rewound through it.

#include "PoseBlend.hpp"

#include "Engine.hpp"
#include "PhysicsObject.hpp"
#include "SceneService.hpp"
#include "support.hpp"
#include "alloc_counter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

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

namespace {

// An Engine that is never started: run_frame runs each frame on this thread.
// One 1-unit cube in zero gravity moves along X at speed, and each frame's
// RenderStepped reads where it is drawn.
struct Drive {
    SimRole role;
    engine_core::Engine engine;
    engine_core::PhysicsObject* body = nullptr;
    engine_core::TaskScheduler::JobId probe{};
    std::vector<float> drawn;

    explicit Drive(float speed) {
        engine_core::DataModel& game = engine.datamodel();
        auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
        REQUIRE(workspace != nullptr);
        REQUIRE_FALSE(workspace->set_gravity(0.0));
        body = &game.create<engine_core::PhysicsObject>();
        REQUIRE_FALSE(body->set_transform(at(0, 10, 0)));
        REQUIRE_FALSE(body->set_size({1, 1, 1}));
        game.set_parent(body->id(), workspace_of(game));
        game.start_simulation();
        engine.set_timing(1.0 / 120.0, 1.0 / 240.0);
        engine.resume();
        REQUIRE_FALSE(body->set_velocity({speed, 0, 0}));
        probe = engine.scheduler().bind(engine_core::Phase::RenderStepped,
                                        [this](double) { drawn.push_back(body->transform().m[12]); });
    }
};

}  // namespace

TEST_CASE("PB5 uneven frames draw evenly spaced", "[pose_blend]") {
    Drive drive(30.f);
    const double walls[] = {0.007, 0.009, 0.008, 0.0075, 0.0085, 0.007, 0.009, 0.008};
    for (int warm = 0; warm < 4; ++warm) {
        drive.engine.run_frame(0.008);
    }
    drive.drawn.clear();
    for (double wall : walls) {
        drive.engine.run_frame(wall);
    }
    REQUIRE(drive.drawn.size() == 8);
    for (std::size_t i = 1; i < drive.drawn.size(); ++i) {
        INFO("frame " << i << ": moved " << drive.drawn[i] - drive.drawn[i - 1]);
        REQUIRE(std::fabs((drive.drawn[i] - drive.drawn[i - 1]) - 30.f * static_cast<float>(walls[i])) < 1e-3f);
    }
}

TEST_CASE("PB6 the sim step reads true poses", "[pose_blend]") {
    Drive drive(30.f);
    std::vector<float> heartbeat;
    drive.engine.scheduler().bind(engine_core::Phase::Heartbeat,
                                  [&](double) { heartbeat.push_back(drive.body->transform().m[12]); });
    drive.engine.run_frame(0.009);
    drive.engine.run_frame(0.009);
    REQUIRE(heartbeat.size() == 2);
    // Each Heartbeat reads where physics left the body: whole substeps.
    const float substep = 30.f / 240.f;
    const float steps = (heartbeat[1] - heartbeat[0]) / substep;
    REQUIRE(std::fabs(steps - std::round(steps)) < 1e-3f);
    // And after the frame the body holds its true pose again.
    REQUIRE(std::fabs(drive.body->transform().m[12] - heartbeat[1]) < 1e-5f);
}

TEST_CASE("PB7 a RenderStepped write to a body wins over the swap-out", "[pose_blend]") {
    Drive drive(30.f);
    drive.engine.run_frame(0.009);
    drive.engine.scheduler().bind(engine_core::Phase::RenderStepped,
                                  [&](double) { drive.body->store_pose(at(100, 10, 0)); });
    drive.engine.run_frame(0.009);
    // The write is kept, carried into the sim's time: within one substep's motion.
    REQUIRE(std::fabs(drive.body->transform().m[12] - 100.f) <= 30.f / 240.f + 1e-4f);
    REQUIRE(drive.body->transform().m[12] >= 100.f);
}

TEST_CASE("PB8 a frame with no substep keeps blending forward", "[pose_blend]") {
    Drive drive(30.f);
    // Two and a half substeps: two run, half a substep is left over.
    drive.engine.run_frame(2.5 / 240.0);
    const int stepped = drive.engine.last_substep_count();
    drive.drawn.clear();
    drive.engine.run_frame(0.001);  // three quarters of a substep left: none runs
    drive.engine.run_frame(0.001);  // still under one
    REQUIRE(stepped == 2);
    REQUIRE(drive.engine.last_substep_count() == 0);
    REQUIRE(drive.drawn.size() == 2);
    REQUIRE(std::fabs((drive.drawn[1] - drive.drawn[0]) - 30.f * 0.001f) < 1e-3f);
}

TEST_CASE("PB9 Stop clears the blend and a destroyed body is skipped", "[pose_blend]") {
    Drive drive(30.f);
    drive.engine.run_frame(0.009);
    REQUIRE_FALSE(drive.engine.datamodel().pose_blend().entries().empty());
    drive.engine.scheduler().unbind(drive.probe);  // the probe reads body
    drive.engine.datamodel().destroy(drive.body->id());
    drive.body = nullptr;
    drive.engine.run_frame(0.009);  // must not touch the dead id
    drive.engine.datamodel().stop_simulation();
    REQUIRE(drive.engine.datamodel().pose_blend().entries().empty());
}

TEST_CASE("PB10 a body turned by Heartbeat after the substeps still blends", "[pose_blend]") {
    Drive drive(30.f);
    float yaw = 0.f;
    drive.engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        yaw += 0.05f;
        const Matrix4 now = drive.body->transform();
        REQUIRE_FALSE(drive.body->set_transform(engine_core::matrix4_multiply(
            at(now.m[12], now.m[13], now.m[14]), engine_core::matrix4_axis_angle({0, 1, 0}, yaw))));
    });
    const double walls[] = {0.007, 0.009, 0.008, 0.0075, 0.0085};
    for (int warm = 0; warm < 4; ++warm) {
        drive.engine.run_frame(0.008);
    }
    drive.drawn.clear();
    for (double wall : walls) {
        drive.engine.run_frame(wall);
    }
    REQUIRE(drive.drawn.size() == 5);
    for (std::size_t i = 1; i < drive.drawn.size(); ++i) {
        INFO("frame " << i << ": moved " << drive.drawn[i] - drive.drawn[i - 1]);
        REQUIRE(std::fabs((drive.drawn[i] - drive.drawn[i - 1]) - 30.f * static_cast<float>(walls[i])) < 2e-3f);
    }
}

TEST_CASE("PB11 a warm capture records without allocating", "[pose_blend][alloc]") {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    SKIP("Debug containers allocate proxies; run in Release");
#endif
    PoseBlend blend;
    const auto capture = [&](float x) {
        blend.begin_capture();
        for (engine_core::InstanceId id = 1; id <= 64; ++id) {
            blend.record(id, PoseBlend::Kind::Body, at(x, 0, 0), at(x + 1.f, 0, 0));
            blend.record(id, PoseBlend::Kind::Body, at(x + 1.f, 0, 0), at(x + 2.f, 0, 0));
        }
        blend.end_capture();
    };
    capture(0.f);  // warm: the first capture sizes its storage
    alloc_counter::arm();
    capture(1.f);
    capture(2.f);
    alloc_counter::disarm();
    REQUIRE(alloc_counter::count() == 0u);
    REQUIRE(blend.entries().size() == 64u);
}

TEST_CASE("PB12 a window write built from the blended read keeps the body's true motion", "[pose_blend]") {
    // A RenderStepped handler that reads a moving body and writes it back
    // turned (`part.Transform = part.Transform * spin`) must not set the
    // body back by the blend's rewind.
    Drive drive(30.f);
    drive.engine.run_frame(0.008);
    drive.engine.run_frame(2.5 / 240.0);  // half a substep left over: alpha 0.5
    std::vector<float> heartbeat;
    drive.engine.scheduler().bind(engine_core::Phase::Heartbeat,
                                  [&](double) { heartbeat.push_back(drive.body->transform().m[12]); });
    const engine_core::TaskScheduler::JobId echo = drive.engine.scheduler().bind(
        engine_core::Phase::RenderStepped,
        [&](double) {
            REQUIRE_FALSE(drive.body->set_transform(engine_core::matrix4_multiply(
                drive.body->transform(), engine_core::matrix4_axis_angle({0, 1, 0}, 0.01))));
        });
    drive.engine.run_frame(2.0 / 240.0);
    drive.engine.scheduler().unbind(echo);
    drive.engine.run_frame(2.0 / 240.0);
    REQUIRE(heartbeat.size() == 2);
    // Two substeps a frame at 30 units/s: 0.25 units between Heartbeats.
    INFO("moved " << heartbeat[1] - heartbeat[0]);
    REQUIRE(std::fabs((heartbeat[1] - heartbeat[0]) - 0.25f) < 1e-3f);
}
