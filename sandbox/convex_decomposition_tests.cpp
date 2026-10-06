// ConvexDecomposition: V-HACD's pieces of a mesh, the memory cache, and the
// studio's queue that writes pieces into a Mesh's file.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ConvexDecomposition.hpp"
#include "Engine.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
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

TEST_CASE("D2 a mesh decomposes once, then comes from the cache", "[decomposition]") {
    engine_core::clear_piece_cache();
    engine_core::Game game;
    // No Path: a Mesh with no file, so only the cache can know its pieces.
    engine_core::Mesh& mesh = game.create<engine_core::Mesh>();
    const Geometry l = ell();
    std::vector<anarchy::amesh::ConvexPiece> known;
    REQUIRE_FALSE(engine_core::known_pieces(mesh, l.points, l.triangles, known));

    const std::uint64_t before = engine_core::decompose_count();
    const auto first = engine_core::pieces_for(mesh, l.points, l.triangles);
    REQUIRE(engine_core::decompose_count() == before + 1);
    const auto second = engine_core::pieces_for(mesh, l.points, l.triangles);
    REQUIRE(engine_core::decompose_count() == before + 1);
    REQUIRE(second.size() == first.size());
    REQUIRE(engine_core::known_pieces(mesh, l.points, l.triangles, known));

    // Other geometry is not the same entry.
    Geometry moved = l;
    moved.points[0].x += 0.25f;
    REQUIRE_FALSE(engine_core::known_pieces(mesh, moved.points, moved.triangles, known));

    // Known to have none is still known.
    engine_core::remember_pieces(moved.points, moved.triangles, {});
    REQUIRE(engine_core::known_pieces(mesh, moved.points, moved.triangles, known));
    REQUIRE(known.empty());
}

TEST_CASE("D3 a decompose stopped before it starts gives no pieces", "[decomposition]") {
    const Geometry l = ell();
    // V-HACD clears a cancel made before its run; the stop is seen again once it runs.
    std::atomic<bool> stop{true};
    REQUIRE(engine_core::decompose(l.points, l.triangles, stop).empty());
    stop = false;
    REQUIRE(engine_core::decompose(l.points, l.triangles, stop).size() >= 2);
}

namespace {

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A stopped game with a resources folder, a Mesh file holding an L, and the queue.
struct QueueRig {
    SimRole role;
    engine_core::Game game;
    engine_core::ConvexDecomposer decomposer;
    std::filesystem::path resources;
    engine_core::Mesh* mesh = nullptr;

    QueueRig() {
        // A folder of its own, so two rigs at once, in this run or another, keep their files.
        static std::atomic<int> rigs{0};
        resources = std::filesystem::temp_directory_path() /
                    ("anarchy-decomposer-test-" + process_id() + "-" + std::to_string(rigs++));
        std::filesystem::remove_all(resources);
        std::filesystem::create_directories(resources);
        game.set_resources_root(resources);
        mesh = &game.create<engine_core::Mesh>();
        REQUIRE_FALSE(mesh->edit_geometry([](anarchy::amesh::Data& data) {
            engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
            engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{0.5f, 2.f, 0.f});
        }));
    }
    ~QueueRig() {
        std::error_code ignored;
        std::filesystem::remove_all(resources, ignored);
    }

    engine_core::PhysicsObject& custom(bool anchored) {
        auto& object = game.create<engine_core::PhysicsObject>();
        REQUIRE_FALSE(object.set_shape(static_cast<int>(engine_core::PhysicsObject::Shape::Custom)));
        REQUIRE_FALSE(object.set_mesh(instance_slot(mesh->id())));
        object.set_anchored(anchored);
        game.set_parent(object.id(), workspace_of(game));
        return object;
    }

    // Waits for the worker, at most ten seconds.
    void wait() {
        for (int tries = 0; tries < 1000 && !decomposer.idle(); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(decomposer.idle());
    }

    bool has_pieces() const {
        std::vector<anarchy::amesh::ConvexPiece> found;
        return mesh->file_pieces(engine_core::kRecipe, found);
    }
};

}  // namespace

TEST_CASE("Q1 a stopped update writes a Custom's pieces into its Mesh's file, once per Mesh", "[decomposition]") {
    QueueRig rig;
    rig.custom(true);
    rig.custom(false);
    const std::uint64_t before = engine_core::decompose_count();
    rig.decomposer.update(rig.game);
    rig.wait();
    REQUIRE_FALSE(rig.has_pieces());
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());
    REQUIRE(engine_core::decompose_count() == before + 1);
    // Already stored: nothing more to do.
    rig.decomposer.update(rig.game);
    REQUIRE(rig.decomposer.idle());
    REQUIRE(engine_core::decompose_count() == before + 1);
}

TEST_CASE("Q2 a result for a Mesh edited or deleted since is dropped", "[decomposition]") {
    QueueRig rig;
    rig.custom(false);
    rig.decomposer.update(rig.game);
    REQUIRE_FALSE(rig.mesh->edit_geometry([](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{1.f, 1.f, 1.f}, Vec3{5.f, 0.f, 0.f});
    }));
    rig.wait();
    // The stale result is dropped, and the new geometry is queued.
    rig.decomposer.update(rig.game);
    REQUIRE_FALSE(rig.has_pieces());
    rig.wait();
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());

    QueueRig other;
    other.custom(false);
    other.decomposer.update(other.game);
    other.game.destroy(other.mesh->id());
    other.wait();
    other.decomposer.update(other.game);
    REQUIRE(other.decomposer.idle());
}

TEST_CASE("Q3 nothing is written while playing; a result waits for Stop", "[decomposition]") {
    QueueRig rig;
    rig.custom(false);
    rig.decomposer.update(rig.game);
    rig.wait();
    const std::string stamp = rig.mesh->file_stamp();
    rig.game.capture_place();
    rig.game.start_simulation();
    // The Engine never updates the queue while playing; Stop brings the result in.
    rig.game.stop_simulation();
    REQUIRE(rig.mesh->file_stamp() == stamp);
    rig.decomposer.update(rig.game);
    REQUIRE(rig.has_pieces());
}

TEST_CASE("Q4 a Mesh that splits into nothing is not queued again until it changes", "[decomposition]") {
    QueueRig rig;
    REQUIRE_FALSE(rig.mesh->edit_geometry([](anarchy::amesh::Data& data) { data = anarchy::amesh::Data{}; }));
    rig.custom(false);
    const std::uint64_t before = engine_core::decompose_count();
    for (int frame = 0; frame < 5; ++frame) {
        rig.decomposer.update(rig.game);
        rig.wait();
    }
    // An empty Mesh has no points: nothing to queue at all.
    REQUIRE(engine_core::decompose_count() == before);
    REQUIRE_FALSE(rig.has_pieces());
}

TEST_CASE("Q5 a stopped Engine writes a Custom's pieces into its Mesh's file", "[decomposition]") {
    const std::filesystem::path resources =
        std::filesystem::temp_directory_path() / ("anarchy-decomposer-test-engine-" + process_id());
    std::filesystem::remove_all(resources);
    std::filesystem::create_directories(resources);
    engine_core::Engine engine;
    engine.start();
    // The studio's Engine is paused whenever the place is stopped.
    REQUIRE(engine.paused());
    engine_core::InstanceId mesh_id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        game.set_resources_root(resources);
        auto& mesh = game.create<engine_core::Mesh>();
        mesh_id = mesh.id();
        REQUIRE_FALSE(mesh.edit_geometry([](anarchy::amesh::Data& data) {
            engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
            engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{0.5f, 2.f, 0.f});
        }));
        auto& object = game.create<engine_core::PhysicsObject>();
        REQUIRE_FALSE(object.set_shape(static_cast<int>(engine_core::PhysicsObject::Shape::Custom)));
        REQUIRE_FALSE(object.set_mesh(instance_slot(mesh.id())));
        game.set_parent(object.id(), workspace_of(game));
    });
    bool stored = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stored && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        engine.on_simulation([&](engine_core::DataModel& game) {
            const auto* mesh = dynamic_cast<const engine_core::Mesh*>(game.instance(mesh_id));
            std::vector<anarchy::amesh::ConvexPiece> found;
            stored = mesh != nullptr && mesh->file_pieces(engine_core::kRecipe, found);
        });
    }
    engine.stop();
    std::error_code ignored;
    std::filesystem::remove_all(resources, ignored);
    REQUIRE(stored);
}

TEST_CASE("Q6 an update queues one Mesh; the next is queued by a later update", "[decomposition]") {
    QueueRig rig;
    rig.custom(false);
    engine_core::Mesh& second = rig.game.create<engine_core::Mesh>();
    REQUIRE_FALSE(second.edit_geometry([](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{3.f, 1.f, 1.f}, Vec3{1.5f, 0.5f, 0.f});
        engine_core::add_box(data, Vec3{1.f, 2.f, 1.f}, Vec3{2.5f, 2.f, 0.f});
    }));
    auto& other = rig.game.create<engine_core::PhysicsObject>();
    REQUIRE_FALSE(other.set_shape(static_cast<int>(engine_core::PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(other.set_mesh(instance_slot(second.id())));
    rig.game.set_parent(other.id(), workspace_of(rig.game));

    const std::uint64_t before = engine_core::decompose_count();
    rig.decomposer.update(rig.game);
    rig.wait();
    REQUIRE(engine_core::decompose_count() == before + 1);
    rig.decomposer.update(rig.game);
    rig.wait();
    REQUIRE(engine_core::decompose_count() == before + 2);
    rig.decomposer.update(rig.game);
    std::vector<anarchy::amesh::ConvexPiece> found;
    REQUIRE(rig.has_pieces());
    REQUIRE(second.file_pieces(engine_core::kRecipe, found));
    // Both stored: nothing more to queue.
    rig.decomposer.update(rig.game);
    REQUIRE(rig.decomposer.idle());
    REQUIRE(engine_core::decompose_count() == before + 2);
}
