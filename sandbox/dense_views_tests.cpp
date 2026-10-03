// Dense views: DenseIdSet, scope bits, the step list, and snapshot membership.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "DenseIdSet.hpp"
#include "Ecs.hpp"
#include "GameObject.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

engine_core::InstanceId test_id(std::uint32_t generation, std::uint32_t slot) {
    return engine_core::make_instance_id(generation, slot);
}

// One Prepare and publish, without engine threads.
void pump_frame(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
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
    game.destroy(folder.id());  // orphans part and deletes folder's entity
    REQUIRE(game.entity_count() == services + 1);
    begin_step(game);
    game.destroy(part_id);
    end_step(game);
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
        part.set_transform(engine_core::matrix4_translation(2.f, 3.f, 4.f));
        REQUIRE(second.entity_count() == base + 1);
        REQUIRE(first.entity_count() == base + 1);
    }
    // The first world outlives the second and still works.
    engine_core::GameObject& later = first.create_game_object();
    later.set_transform(engine_core::matrix4_translation(5.f, 6.f, 7.f));
    REQUIRE(later.transform().m[14] == 7.f);
    REQUIRE(first.entity_count() == base + 2);
}

TEST_CASE("flecs leaves the process timer resolution alone", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    // flecs raises the Windows timer to 1 ms at each world's start while this
    // flag is set. The engine paces frames around the default tick instead.
    REQUIRE((ecs_os_api.flags_ & EcsOsApiHighResolutionTimer) == 0u);
}

TEST_CASE("a destroyed GameObject reads zero", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    part.set_transform(engine_core::matrix4_translation(2.f, 2.f, 2.f));
    game.destroy(part.id());
    REQUIRE(part.transform().m[0] == 0.f);  // the zero matrix, not identity
    REQUIRE(part.transform().m[12] == 0.f);  // not the translation it had
    REQUIRE(part.position().y == 0.f);
}

TEST_CASE("undo and Stop keep spatial values and flags", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    game.set_parent(id, workspace_of(game));
    part.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f));
    game.set_simulated(id, true);
    game.set_visual_only(id, true);
    begin_step(game);
    game.destroy(id);
    end_step(game);
    game.history().undo();
    REQUIRE(game.alive(id));
    REQUIRE(game.simulated(id));
    REQUIRE(game.visual_only(id));
    REQUIRE(game.game_object(id)->transform().m[13] == 2.f);

    game.capture_place();
    game.start_simulation();
    game.set_simulated(id, false);
    game.game_object(id)->set_transform(engine_core::matrix4_translation(5.f, 5.f, 5.f));
    game.stop_simulation();
    REQUIRE(game.simulated(id));
    REQUIRE(game.game_object(id)->transform().m[13] == 2.f);
}

TEST_CASE("scope tags follow the tree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    const engine_core::InstanceId storage = game.scene_service("Storage");
    REQUIRE(game.in_game(ws));
    REQUIRE_FALSE(game.in_workspace(ws));  // the service is not inside itself
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    REQUIRE_FALSE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));
    game.set_parent(id, ws);
    REQUIRE(game.in_game(id));
    REQUIRE(game.in_workspace(id));
    game.set_parent(id, storage);
    REQUIRE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));
    game.set_parent(id, engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));
    REQUIRE_FALSE(game.in_game(engine_core::make_instance_id(9, 999)));  // dead
}

TEST_CASE("scope tags flip a whole subtree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& part = game.create_game_object();
    engine_core::GameObject& nested = game.create_game_object();
    game.set_parent(part.id(), folder.id());
    game.set_parent(nested.id(), part.id());
    REQUIRE_FALSE(game.in_workspace(nested.id()));
    game.set_parent(folder.id(), workspace_of(game));
    REQUIRE(game.in_workspace(folder.id()));
    REQUIRE(game.in_workspace(part.id()));
    REQUIRE(game.in_workspace(nested.id()));
    game.set_parent(folder.id(), engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(part.id()));
    REQUIRE_FALSE(game.in_workspace(nested.id()));
}

TEST_CASE("destroy clears scope; undo restores it", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& parent = game.create_game_object();
    engine_core::GameObject& child = game.create_game_object();
    game.set_parent(parent.id(), workspace_of(game));
    game.set_parent(child.id(), parent.id());
    const engine_core::InstanceId parent_id = parent.id();
    const engine_core::InstanceId child_id = child.id();
    // destroy, not destroy_tree: the child lives on, out of the tree.
    begin_step(game);
    game.destroy(parent_id);
    end_step(game);
    REQUIRE(game.alive(child_id));
    REQUIRE_FALSE(game.in_game(child_id));
    REQUIRE_FALSE(game.in_workspace(child_id));
    game.history().undo();
    REQUIRE(game.alive(parent_id));
    REQUIRE(game.parent(child_id) == parent_id);
    REQUIRE(game.in_workspace(parent_id));
    REQUIRE(game.in_workspace(child_id));
}

TEST_CASE("Stop restores scope", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::GameObject& authored = game.create_game_object();
    game.set_parent(authored.id(), ws);
    game.capture_place();
    game.start_simulation();
    engine_core::GameObject& session = game.create_game_object();
    game.set_parent(session.id(), ws);
    game.set_parent(authored.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(game.in_workspace(authored.id()));
    game.stop_simulation();
    REQUIRE(game.in_workspace(authored.id()));
    REQUIRE(game.in_game(ws));
}

TEST_CASE("Stop takes scope from an instance that was unparented at capture", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& loose = game.create_game_object();
    engine_core::GameObject& child = game.create_game_object();
    game.set_parent(child.id(), loose.id());
    game.capture_place();
    game.start_simulation();
    game.set_parent(loose.id(), workspace_of(game));
    REQUIRE(game.in_workspace(child.id()));
    game.stop_simulation();
    REQUIRE(game.parent(loose.id()) == engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(loose.id()));
    REQUIRE_FALSE(game.in_workspace(child.id()));
}

TEST_CASE("a Spinner steps only while it is under game", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    Spinner& spinner = game.create<Spinner>();
    const engine_core::InstanceId id = spinner.id();
    game.step_instances(0.25);
    REQUIRE(spinner.degrees() == 0.0);
    game.set_parent(id, ws);
    game.step_instances(0.25);  // 90 degrees a second
    REQUIRE(spinner.degrees() == 22.5);
    game.set_parent(id, engine_core::DataModel::kNoParent);
    game.step_instances(0.25);
    REQUIRE(spinner.degrees() == 22.5);
    game.set_parent(id, game.scene_service("Storage"));  // anywhere under game steps
    game.step_instances(0.25);
    REQUIRE(spinner.degrees() == 45.0);
    game.destroy(id);
    game.step_instances(0.25);
    REQUIRE_FALSE(game.alive(id));
    REQUIRE(game.stepper_count() == 0);
}

TEST_CASE("undo revives a Spinner that still steps", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    Spinner& spinner = game.create<Spinner>();
    const engine_core::InstanceId id = spinner.id();
    game.set_parent(id, workspace_of(game));
    begin_step(game);
    game.destroy(id);
    end_step(game);
    REQUIRE(game.stepper_count() == 0);
    game.history().undo();
    REQUIRE(game.alive(id));
    REQUIRE(game.stepper_count() == 1);
    auto* revived = dynamic_cast<Spinner*>(game.instance(id));
    REQUIRE(revived != nullptr);
    const double before = revived->degrees();
    game.step_instances(0.25);
    REQUIRE(revived->degrees() == before + 22.5);
}

TEST_CASE("Stop rebuilds authored instances that play destroyed, in their own slots", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId part_id = part.id();
    game.set_parent(part_id, ws);
    part.set_transform(engine_core::matrix4_translation(0.25f, 0.5f, 0.75f));
    game.set_simulated(part_id, true);
    game.set_visual_only(part_id, true);
    Spinner& spinner = game.create<Spinner>();
    const engine_core::InstanceId spinner_id = spinner.id();
    game.set_parent(spinner_id, ws);
    game.capture_place();

    game.start_simulation();
    game.destroy(part_id);
    game.destroy(spinner_id);
    // A captured instance's slot waits for Stop, so new instances made in play
    // go elsewhere, and Stop rebuilds the authored ones in their own slots.
    engine_core::DataModel& squatter = game.create();
    engine_core::DataModel& other = game.create();
    game.set_parent(squatter.id(), game.scene_service("Storage"));
    game.set_parent(other.id(), game.scene_service("Storage"));
    const auto slot_of = [](engine_core::InstanceId id) { return engine_core::id_slot(id); };
    const bool reused = slot_of(squatter.id()) == slot_of(part_id) || slot_of(squatter.id()) == slot_of(spinner_id) ||
                        slot_of(other.id()) == slot_of(part_id) || slot_of(other.id()) == slot_of(spinner_id);
    REQUIRE_FALSE(reused);
    game.stop_simulation();

    REQUIRE(game.alive(part_id));
    REQUIRE(game.simulated(part_id));
    REQUIRE(game.visual_only(part_id));
    REQUIRE(game.in_workspace(part_id));
    REQUIRE(game.game_object(part_id)->transform().m[13] == 0.5f);
    REQUIRE(game.alive(spinner_id));
    REQUIRE(game.in_game(spinner_id));
    REQUIRE(game.stepper_count() == 1);
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part_id);
    REQUIRE(row != nullptr);
    REQUIRE(row->world.m[14] == 0.75f);
}

TEST_CASE("plain instances never step", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), workspace_of(game));
    REQUIRE(game.stepper_count() == 0);
    Spinner& spinner = game.create<Spinner>();
    REQUIRE(game.stepper_count() == 0);  // not under game yet
    game.set_parent(spinner.id(), folder.id());
    REQUIRE(game.stepper_count() == 1);
}

TEST_CASE("physics moves simulated bodies only", "[dense][physics]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& moving = game.create_game_object();
    engine_core::GameObject& idle = game.create_game_object();
    engine_core::GameObject& shown = game.create_game_object();
    engine_core::GameObject& still = game.create_game_object();
    for (engine_core::GameObject* body : {&moving, &idle, &shown, &still}) {
        game.set_parent(body->id(), workspace_of(game));
    }
    moving.set_linear_velocity(4.f, 0.f, -2.f);
    idle.set_linear_velocity(4.f, 0.f, 0.f);
    shown.set_linear_velocity(4.f, 0.f, 0.f);
    game.set_simulated(moving.id(), true);
    game.set_simulated(shown.id(), true);
    game.set_visual_only(shown.id(), true);
    game.set_simulated(still.id(), true);  // zero velocity
    game.invalidations().clear();
    game.integrate_simulated(0.5);
    REQUIRE(moving.transform().m[12] == 2.f);
    REQUIRE(moving.transform().m[14] == -1.f);
    REQUIRE(idle.transform().m[12] == 0.f);
    REQUIRE(shown.transform().m[12] == 0.f);
    REQUIRE(still.transform().m[12] == 0.f);
    REQUIRE(game.invalidations().size() == 1);
}

TEST_CASE("only Workspace GameObjects have snapshot rows", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);  // unparented
    game.set_parent(id, workspace_of(game));
    pump_frame(pump, game);
    REQUIRE(pump.find(id) != nullptr);
    game.set_parent(id, game.scene_service("Storage"));
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
    game.set_parent(id, workspace_of(game));
    game.set_parent(id, engine_core::DataModel::kNoParent);  // in and out in one frame
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
}

TEST_CASE("a row arrives complete after edits made outside Workspace", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), game.scene_service("Storage"));
    pump_frame(pump, game);
    part.set_transform(engine_core::matrix4_translation(0.25f, 3.f, 4.f));  // no row to patch yet
    pump_frame(pump, game);
    REQUIRE(pump.find(part.id()) == nullptr);
    game.set_parent(part.id(), workspace_of(game));
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->world.m[12] == 0.25f);
    REQUIRE(row->world.m[13] == 3.f);
}

TEST_CASE("a move within Workspace keeps the row", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), workspace_of(game));
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), workspace_of(game));
    part.set_transform(engine_core::matrix4_translation(0.1f, 0.2f, 0.3f));
    pump_frame(pump, game);
    game.invalidations().clear();
    game.set_parent(part.id(), folder.id());
    REQUIRE(game.invalidations().size() == 0);  // scope unchanged: no Ancestry note
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->world.m[13] == 0.2f);
}

TEST_CASE("destroy_tree clears rows and steppers", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& a = game.create_game_object();
    engine_core::GameObject& b = game.create_game_object();
    Spinner& spinner = game.create<Spinner>();
    game.set_parent(folder.id(), workspace_of(game));
    game.set_parent(a.id(), folder.id());
    game.set_parent(b.id(), a.id());
    game.set_parent(spinner.id(), folder.id());
    const engine_core::InstanceId a_id = a.id();
    const engine_core::InstanceId b_id = b.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) != nullptr);
    REQUIRE(pump.find(b_id) != nullptr);
    REQUIRE(game.stepper_count() == 1);
    game.destroy_tree(folder.id());
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) == nullptr);
    REQUIRE(pump.find(b_id) == nullptr);
    REQUIRE(game.stepper_count() == 0);
}

TEST_CASE("overflow resync keeps Workspace membership", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& shown = game.create_game_object();
    engine_core::GameObject& stored = game.create_game_object();
    game.set_parent(shown.id(), workspace_of(game));
    game.set_parent(stored.id(), game.scene_service("Storage"));
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    for (std::size_t i = 0; i <= engine_core::DataModel::kMaxInvalidations; ++i) {
        moved.m[12] = static_cast<float>(i + 1);  // an equal write would skip its note
        shown.set_transform(moved);
    }
    REQUIRE(game.invalidations().overflow());
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(shown.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->world.m[12] == moved.m[12]);
    REQUIRE(pump.find(stored.id()) == nullptr);
}

TEST_CASE("Stop leaves rows for the restored Workspace", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& authored = game.create_game_object();
    engine_core::GameObject& loose = game.create_game_object();
    game.set_parent(authored.id(), workspace_of(game));
    game.capture_place();
    game.start_simulation();
    game.set_parent(authored.id(), game.scene_service("Storage"));
    game.set_parent(loose.id(), workspace_of(game));
    pump_frame(pump, game);
    REQUIRE(pump.find(authored.id()) == nullptr);
    REQUIRE(pump.find(loose.id()) != nullptr);
    game.stop_simulation();
    pump_frame(pump, game);
    REQUIRE(pump.find(authored.id()) != nullptr);
    REQUIRE(pump.find(loose.id()) == nullptr);  // unparented again in the place
}

TEST_CASE("Instance.new GameObject renders once parented into Workspace", "[dense][member][lua]") {
    ScriptRig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    add_script(rig.game, "Spawner", R"(
        local part = Instance.new("GameObject")
        part.Name = "Spawned"
        part.Parent = workspace
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    const engine_core::InstanceId id = rig.game.find_first_child(workspace_of(rig.game), "Spawned");
    REQUIRE(id != 0);
    pump_frame(pump, rig.game);
    REQUIRE(pump.find(id) != nullptr);
}
