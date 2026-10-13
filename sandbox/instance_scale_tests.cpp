// Instance scale: ids past 16 bits of slot, capacity, retired slots, and the
// per-slot tables that grow with a place instead of being sized for the cap.

#include "support.hpp"

#include "Folder.hpp"
#include "GameObject.hpp"
#include "Project.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {

using engine_core::DataModel;
using engine_core::InstanceId;

// Plain instances under nothing: the cheapest way to take slots.
std::vector<InstanceId> take_slots(DataModel& game, std::size_t count) {
    std::vector<InstanceId> made;
    made.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        made.push_back(game.create().id());
    }
    return made;
}

}  // namespace

TEST_CASE("IS1 a place holds instances in slots past 65,535 and finds each by its id", "[IS1][scale]") {
    SimRole role;
    engine_core::Game game;
    game.history().set_enabled(false);
    const std::vector<InstanceId> made = take_slots(game, 70'000);
    std::uint32_t highest = 0;
    for (const InstanceId id : made) {
        highest = std::max(highest, engine_core::id_slot(id));
    }
    REQUIRE(highest > 0xffffu);
    for (const InstanceId id : made) {
        REQUIRE(game.alive(id));
        REQUIRE(game.instance(id) != nullptr);
        REQUIRE(game.instance(id)->id() == id);
    }
    // A high slot is reused with a new generation, and its old id stays dead.
    const InstanceId old = made.back();
    game.destroy(old);
    REQUIRE_FALSE(game.alive(old));
    const InstanceId reused = game.create().id();
    REQUIRE(engine_core::id_slot(reused) == engine_core::id_slot(old));
    REQUIRE(engine_core::id_generation(reused) == engine_core::id_generation(old) + 1);
    REQUIRE(game.alive(reused));
    REQUIRE_FALSE(game.alive(old));
    REQUIRE(game.instance(old) == nullptr);
}

TEST_CASE("IS2 the instance capacity counts down, clamps, and names itself when full", "[IS2][scale]") {
    SimRole role;
    engine_core::Game game;
    game.history().set_enabled(false);
    REQUIRE(game.instance_capacity() == DataModel::kMaxInstances);
    REQUIRE(DataModel::kMaxInstances >= 1'000'000);
    const std::size_t in_use = DataModel::kMaxInstances - game.room_left();
    game.set_instance_capacity(in_use + 3);
    REQUIRE(game.room_left() == 3);
    take_slots(game, 3);
    REQUIRE(game.room_left() == 0);
    try {
        game.create();
        FAIL("a full place must refuse a create");
    } catch (const engine_core::InstanceCapacityError& full) {
        REQUIRE(std::string(full.what()).find(std::to_string(in_use + 3)) != std::string::npos);
    }
    // Never below the slots in use, never above the cap.
    game.set_instance_capacity(1);
    REQUIRE(game.instance_capacity() == in_use + 3);
    game.set_instance_capacity(DataModel::kMaxInstances * 2);
    REQUIRE(game.instance_capacity() == DataModel::kMaxInstances);
    REQUIRE(game.room_left() > 0);
}

TEST_CASE("IS3 a slot whose generation runs out is retired, also after Stop", "[IS3][scale]") {
    SimRole role;
    engine_core::Game game;
    game.history().set_enabled(false);
    game.capture_place();
    game.start_simulation();
    // Creates reuse the last freed slot, so one slot cycles through every generation.
    InstanceId id = game.create().id();
    const std::uint32_t slot = engine_core::id_slot(id);
    while (engine_core::id_generation(id) < engine_core::kMaxGeneration) {
        game.destroy(id);
        id = game.create().id();
        REQUIRE(engine_core::id_slot(id) == slot);
    }
    const InstanceId last_life = id;
    game.destroy(last_life);
    // Retired: the next create takes another slot.
    const InstanceId next = game.create().id();
    REQUIRE(engine_core::id_slot(next) != slot);
    game.stop_simulation();
    // Stop rebuilds the free list; the retired slot must not come back, or
    // last_life's id could be issued again.
    for (const InstanceId made : take_slots(game, 64)) {
        REQUIRE(engine_core::id_slot(made) != slot);
        REQUIRE(made != last_life);
    }
    REQUIRE_FALSE(game.alive(last_life));
}

TEST_CASE("IS4 the render snapshot carries more rows than its starting size", "[IS4][scale]") {
    SimRole role;
    engine_core::Game game;
    game.history().set_enabled(false);
    engine_core::SnapshotPump pump;
    pump.reserve(DataModel::kMaxInstances);
    const InstanceId ws = workspace_of(game);
    const std::size_t count = engine_core::SnapshotPump::kInitialRows + 5'000;
    std::vector<InstanceId> parts;
    parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        engine_core::GameObject& part = game.create_game_object();
        game.set_parent(part.id(), ws);
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(i), 0.f, 0.f));
        parts.push_back(part.id());
    }
    pump.prepare_copy(game);
    pump.publish();
    const auto& rows = pump.front().instances;
    std::unordered_set<InstanceId> seen;
    for (const auto& row : rows) {
        seen.insert(row.id);
    }
    for (const InstanceId id : parts) {
        REQUIRE(seen.count(id) == 1);
    }
    // A move of one of the late rows still reaches the snapshot.
    game.game_object(parts.back())->set_transform(engine_core::matrix4_translation(-1.f, 2.f, 3.f));
    pump.prepare_copy(game);
    pump.publish();
    bool moved = false;
    for (const auto& row : pump.front().instances) {
        if (row.id == parts.back()) {
            moved = row.world.m[13] == 2.f;
        }
    }
    REQUIRE(moved);
}

TEST_CASE("IS5 a project read past the instance limit is an error", "[IS5][scale][project]") {
    REQUIRE(engine_core::Project::instance_limit() == DataModel::kMaxInstances);
    engine_core::Project::set_instance_limit(0);
    REQUIRE(engine_core::Project::instance_limit() == 1);
    engine_core::Project::set_instance_limit(DataModel::kMaxInstances + 10);
    REQUIRE(engine_core::Project::instance_limit() == DataModel::kMaxInstances);
}

TEST_CASE("IS6 shared rows: a copy shares chunks until a write, which copies only that chunk", "[IS6][scale]") {
    using Rows = engine_core::SharedRows<int, 4>;
    Rows rows;
    for (int i = 0; i < 10; ++i) {
        rows.push_back(i);
    }
    REQUIRE(rows.size() == 10);
    REQUIRE(rows.chunk_count() == 3);
    const Rows copy = rows;
    for (std::size_t k = 0; k < rows.chunk_count(); ++k) {
        REQUIRE(copy.chunk(k) == rows.chunk(k));
    }
    rows.at_mut(5) = 50;
    // The written row's chunk is the writer's own; the copy keeps the old one.
    REQUIRE(rows[5] == 50);
    REQUIRE(copy[5] == 5);
    REQUIRE(rows.chunk(1) != copy.chunk(1));
    REQUIRE(rows.chunk(0) == copy.chunk(0));
    REQUIRE(rows.chunk(2) == copy.chunk(2));
    // Popping across a chunk boundary and pushing back keeps every row.
    rows.pop_back();
    rows.pop_back();
    REQUIRE(rows.size() == 8);
    REQUIRE(rows.chunk_count() == 2);
    rows.push_back(80);
    REQUIRE(rows.size() == 9);
    REQUIRE(rows[8] == 80);
    std::vector<int> seen(rows.begin(), rows.end());
    REQUIRE(seen == std::vector<int>{0, 1, 2, 3, 4, 50, 6, 7, 80});
    REQUIRE(std::vector<int>(copy.begin(), copy.end()) == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    rows.resize(2);
    REQUIRE(rows.size() == 2);
    rows.resize(5);
    REQUIRE(rows[4] == 0);
}

TEST_CASE("IS7 a published frame keeps its rows while the pump moves on, and unchanged chunks are shared", "[IS7][scale]") {
    SimRole role;
    engine_core::Game game;
    game.history().set_enabled(false);
    engine_core::SnapshotPump pump;
    pump.reserve(DataModel::kMaxInstances);
    const InstanceId ws = workspace_of(game);
    std::vector<InstanceId> parts;
    for (int i = 0; i < 1000; ++i) {
        engine_core::GameObject& part = game.create_game_object();
        game.set_parent(part.id(), ws);
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(i), 0.f, 0.f));
        parts.push_back(part.id());
    }
    pump.prepare_copy(game);
    pump.publish();
    const engine_core::VisualRows held = pump.front().instances;
    // One part moves: the next frame copies only that row's chunk.
    game.game_object(parts[500])->set_transform(engine_core::matrix4_translation(-5.f, 1.f, 0.f));
    pump.prepare_copy(game);
    pump.publish();
    const engine_core::VisualRows& now = pump.front().instances;
    REQUIRE(now.size() == held.size());
    std::size_t differing = 0;
    for (std::size_t k = 0; k < now.chunk_count(); ++k) {
        differing += now.chunk(k) != held.chunk(k);
    }
    REQUIRE(differing == 1);
    // The held frame still reads as it was.
    for (const auto& row : held) {
        if (row.id == parts[500]) {
            REQUIRE(row.world.m[12] == 500.f);
        }
    }
    for (const auto& row : now) {
        if (row.id == parts[500]) {
            REQUIRE(row.world.m[12] == -5.f);
        }
    }
}
