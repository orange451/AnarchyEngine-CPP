// Dense views: DenseIdSet, scope bits, the step list, and snapshot membership.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "DenseIdSet.hpp"
#include "GameObject.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

engine_core::InstanceId test_id(std::uint32_t generation, std::uint32_t slot) {
    return engine_core::make_instance_id(generation, slot);
}

}  // namespace

TEST_CASE("DenseIdSet inserts, finds, and rejects duplicates", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 5);
    const engine_core::InstanceId b = test_id(1, 9);
    REQUIRE(set.size() == 0);
    REQUIRE_FALSE(set.contains(a));
    REQUIRE(set.position(a) == -1);
    REQUIRE(set.insert(a));
    REQUIRE(set.insert(b));
    REQUIRE_FALSE(set.insert(a));
    REQUIRE(set.size() == 2);
    REQUIRE(set.contains(a));
    REQUIRE(set.position(a) == 0);
    REQUIRE(set.position(b) == 1);
    REQUIRE(set.ids()[0] == a);
    REQUIRE(set.ids()[1] == b);
}

TEST_CASE("DenseIdSet erase swaps the last id in and reports it", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 1);
    const engine_core::InstanceId b = test_id(1, 2);
    const engine_core::InstanceId c = test_id(1, 3);
    set.insert(a);
    set.insert(b);
    set.insert(c);
    engine_core::InstanceId moved = 123;
    REQUIRE(set.erase(a, &moved) == 0);
    REQUIRE(moved == c);
    REQUIRE(set.position(c) == 0);
    REQUIRE(set.position(b) == 1);
    REQUIRE(set.size() == 2);
    // Erasing the last element swaps nothing.
    REQUIRE(set.erase(b, &moved) == 1);
    REQUIRE(moved == 0);
    // Erasing an absent id reports -1 and no swap.
    REQUIRE(set.erase(a, &moved) == -1);
    REQUIRE(moved == 0);
    REQUIRE(set.size() == 1);
}

TEST_CASE("DenseIdSet misses a stale generation on a reused slot", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId old_id = test_id(1, 7);
    const engine_core::InstanceId new_id = test_id(2, 7);
    set.insert(old_id);
    REQUIRE_FALSE(set.contains(new_id));
    REQUIRE(set.position(new_id) == -1);
    REQUIRE(set.erase(new_id) == -1);
    REQUIRE(set.contains(old_id));
    set.erase(old_id);
    set.insert(new_id);
    REQUIRE_FALSE(set.contains(old_id));
    REQUIRE(set.contains(new_id));
}

TEST_CASE("DenseIdSet clear empties and forgets positions", "[dense]") {
    engine_core::DenseIdSet set;
    set.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId a = test_id(1, 4);
    set.insert(a);
    set.clear();
    REQUIRE(set.size() == 0);
    REQUIRE_FALSE(set.contains(a));
    REQUIRE(set.insert(a));
    REQUIRE(set.position(a) == 0);
}

TEST_CASE("every live instance has one entity", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    const std::size_t services = game.entity_count();
    REQUIRE(services > 0);  // the service tree already has entities
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId part_id = part.id();
    REQUIRE(game.entity_count() == services + 2);
    game.set_parent(part_id, folder.id());
    game.history().end_gesture();
    game.destroy(folder.id());  // orphans part and deletes folder's entity
    game.history().end_gesture();
    REQUIRE(game.entity_count() == services + 1);
    game.destroy(part_id);
    game.history().end_gesture();
    REQUIRE(game.entity_count() == services);
    game.history().undo();  // revives part through the place-restore path
    REQUIRE(game.alive(part_id));
    REQUIRE(game.entity_count() == services + 1);
}

TEST_CASE("two DataModels keep separate worlds", "[dense][entity]") {
    SimRole role;
    engine_core::Game first;
    const std::size_t base = first.entity_count();
    first.create_game_object();
    {
        engine_core::Game second;
        REQUIRE(second.entity_count() == base);
        engine_core::GameObject& part = second.create_game_object();
        part.set_size(2.f, 3.f, 4.f);
        REQUIRE(second.entity_count() == base + 1);
        REQUIRE(first.entity_count() == base + 1);
    }
    // The first world outlives the second and still works.
    engine_core::GameObject& later = first.create_game_object();
    later.set_size(5.f, 6.f, 7.f);
    float size[3] = {};
    REQUIRE(later.copy_size(size));
    REQUIRE(size[2] == 7.f);
    REQUIRE(first.entity_count() == base + 2);
}
