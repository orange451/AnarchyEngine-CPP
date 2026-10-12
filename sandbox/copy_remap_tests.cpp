// Copy, Paste, Duplicate, and instance files: references that point inside
// what one action copies are remapped to the copies.

#include "support.hpp"

#include "Assemblies.hpp"
#include "Constraint.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "InstanceFile.hpp"
#include "PhysicsObject.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using engine_core::CopiedNode;
using engine_core::CopyMap;
using engine_core::DataModel;
using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::PhysicsObject;
using engine_core::RigidConstraint;
using engine_core::WeldConstraint;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

template <class T>
T& join(DataModel& game, InstanceId a, InstanceId b, InstanceId parent) {
    T& constraint = game.create<T>();
    REQUIRE_FALSE(constraint.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(b)));
    game.set_parent(constraint.id(), parent);
    return constraint;
}

GameObject& named_part(DataModel& game, const char* name, InstanceId parent) {
    GameObject& part = game.create_game_object();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), parent);
    return part;
}

// What Duplicate does: each top-level instance its own group, one map for all.
std::vector<InstanceId> duplicate(DataModel& game, const std::vector<InstanceId>& ids) {
    std::vector<std::vector<CopiedNode>> groups;
    std::vector<InstanceId> parents;
    for (const InstanceId id : ids) {
        groups.push_back({engine_core::copy_tree(game, id)});
        parents.push_back(game.parent(id));
    }
    engine_core::AssemblySuspend suspend(game);
    CopyMap map;
    std::vector<InstanceId> made;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        REQUIRE(engine_core::paste_copies(game, groups[i], parents[i], &made, nullptr, &map));
    }
    engine_core::remap_references(game, map);
    game.assemblies().follow_on_resume();
    return made;
}

template <class T>
T* as(DataModel& game, InstanceId id) {
    return dynamic_cast<T*>(game.instance(id));
}

}  // namespace

TEST_CASE("CR1 duplicating two welded parts and their weld welds the copies, through undo and redo",
          "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    GameObject& b = named_part(game, "B", workspace_of(game));
    WeldConstraint& w = join<WeldConstraint>(game, a.id(), b.id(), workspace_of(game));
    game.history().reset_waypoints();
    begin_step(game, "Duplicate");
    const std::vector<InstanceId> made = duplicate(game, {a.id(), b.id(), w.id()});
    end_step(game);
    REQUIRE(made.size() == 3);
    const auto check = [&] {
        auto* copy = as<WeldConstraint>(game, made[2]);
        REQUIRE(copy != nullptr);
        REQUIRE(copy->pv_instance1_id() == made[0]);
        REQUIRE(copy->pv_instance2_id() == made[1]);
    };
    check();
    REQUIRE(w.pv_instance1_id() == a.id());
    game.history().undo();
    REQUIRE_FALSE(game.alive(made[2]));
    game.history().redo();
    check();
}

TEST_CASE("CR2 a constraint copied alone still names the originals", "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    GameObject& b = named_part(game, "B", workspace_of(game));
    WeldConstraint& w = join<WeldConstraint>(game, a.id(), b.id(), workspace_of(game));
    const std::vector<InstanceId> made = duplicate(game, {w.id()});
    auto* copy = as<WeldConstraint>(game, made[0]);
    REQUIRE(copy->pv_instance1_id() == a.id());
    REQUIRE(copy->pv_instance2_id() == b.id());
}

TEST_CASE("CR3 a RigidConstraint copied without its sub loses the sub end", "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    GameObject& b = named_part(game, "B", workspace_of(game));
    join<RigidConstraint>(game, a.id(), b.id(), a.id());
    const std::vector<InstanceId> made = duplicate(game, {a.id()});
    const InstanceId copy_rigid = game.first_child(made[0]);
    auto* copy = as<RigidConstraint>(game, copy_rigid);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->pv_instance1_id() == made[0]);
    REQUIRE(copy->pv_instance2_id() == 0);
    REQUIRE(copy->pv_instance2_ref().guid().empty());
}

TEST_CASE("CR5 an instance file keeps its links when inserted, and an old file still loads", "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    TempDir dir;
    engine_core::Folder& model = game.create<engine_core::Folder>();
    game.set_parent(model.id(), workspace_of(game));
    GameObject& a = named_part(game, "A", model.id());
    GameObject& b = named_part(game, "B", model.id());
    join<WeldConstraint>(game, a.id(), b.id(), model.id());
    const std::filesystem::path path = dir / "model.aeinst";
    std::string error;
    REQUIRE(engine_core::save_instance_file(path, {engine_core::copy_tree(game, model.id())}, error));
    std::vector<CopiedNode> roots;
    REQUIRE(engine_core::load_instance_file(path, roots, error));
    CopyMap map;
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(game, roots, workspace_of(game), &made, nullptr, &map));
    engine_core::remap_references(game, map);
    const InstanceId inserted_a = game.find_first_child(made[0], "A");
    const InstanceId inserted_b = game.find_first_child(made[0], "B");
    InstanceId inserted_weld = 0;
    for (InstanceId child = game.first_child(made[0]); child != 0; child = game.next_sibling(child)) {
        if (as<WeldConstraint>(game, child) != nullptr) {
            inserted_weld = child;
        }
    }
    auto* weld = as<WeldConstraint>(game, inserted_weld);
    REQUIRE(weld != nullptr);
    REQUIRE(weld->pv_instance1_id() == inserted_a);
    REQUIRE(weld->pv_instance2_id() == inserted_b);

    // Version 1 with no "guid" keys: it loads, and nothing is remapped.
    engine_core::JsonValue json = engine_core::write_instance_file(roots);
    roots.clear();
    REQUIRE(engine_core::read_instance_file(json, roots, error));
    const std::string text = engine_core::write_json(json);
    REQUIRE(text.find("\"guid\"") != std::string::npos);
    const std::string old = "{\"format\": \"aeinst\", \"version\": 1, \"roots\": "
                            "[{\"class\": \"Folder\", \"name\": \"Old\"}]}";
    engine_core::JsonValue parsed;
    REQUIRE(engine_core::parse_json(old, parsed, error));
    roots.clear();
    REQUIRE(engine_core::read_instance_file(parsed, roots, error));
    REQUIRE(roots.size() == 1);
    REQUIRE(roots[0].guid.empty());
}

TEST_CASE("CR6 a pasted RigidConstraint pair follows only its own copy, and undoes in one step",
          "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    a.set_transform(engine_core::matrix4_translation(0.f, 5.f, 0.f));
    // An anchored body makes A, and each copy of it, the root.
    PhysicsObject& body = game.create<PhysicsObject>();
    body.set_anchored(true);
    game.set_parent(body.id(), a.id());
    GameObject& b = named_part(game, "B", workspace_of(game));
    join<RigidConstraint>(game, a.id(), b.id(), workspace_of(game));
    REQUIRE(b.transform().m[13] == 5.f);
    const std::vector<CopiedNode> copies = {engine_core::copy_tree(game, a.id()),
                                            engine_core::copy_tree(game, b.id()),
                                            engine_core::copy_tree(game, game.find_first_child(workspace_of(game), "RigidConstraint"))};
    game.history().reset_waypoints();
    begin_step(game, "Paste");
    std::vector<InstanceId> made;
    {
        engine_core::AssemblySuspend suspend(game);
        CopyMap map;
        REQUIRE(engine_core::paste_copies(game, copies, workspace_of(game), &made, nullptr, &map));
        engine_core::remap_references(game, map);
        game.assemblies().follow_on_resume();
    }
    end_step(game);
    REQUIRE(made.size() == 3);
    game.game_object(made[0])->set_transform(engine_core::matrix4_translation(9.f, 9.f, 9.f));
    REQUIRE(game.game_object(made[1])->transform().m[12] == 9.f);
    REQUIRE(b.transform().m[13] == 5.f);
    REQUIRE(b.transform().m[12] == 0.f);
    game.history().undo();
    REQUIRE_FALSE(game.alive(made[0]));
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("CR7 a weld pasted onto two targets names each target's own copies", "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    GameObject& b = named_part(game, "B", workspace_of(game));
    WeldConstraint& w = join<WeldConstraint>(game, a.id(), b.id(), workspace_of(game));
    const std::vector<CopiedNode> copies = {engine_core::copy_tree(game, a.id()), engine_core::copy_tree(game, b.id()),
                                            engine_core::copy_tree(game, w.id())};
    engine_core::Folder& one = game.create<engine_core::Folder>();
    game.set_parent(one.id(), workspace_of(game));
    engine_core::Folder& two = game.create<engine_core::Folder>();
    game.set_parent(two.id(), workspace_of(game));
    for (const InstanceId target : {one.id(), two.id()}) {
        CopyMap map;
        std::vector<InstanceId> made;
        REQUIRE(engine_core::paste_copies(game, copies, target, &made, nullptr, &map));
        engine_core::remap_references(game, map);
        auto* weld = as<WeldConstraint>(game, made[2]);
        REQUIRE(weld->pv_instance1_id() == made[0]);
        REQUIRE(weld->pv_instance2_id() == made[1]);
        REQUIRE(game.parent(weld->pv_instance1_id()) == target);
    }
}

TEST_CASE("CR8 a RigidConstraint copied with only its second end loses its first", "[assembly][remap]") {
    SimRole role;
    engine_core::Game game;
    GameObject& a = named_part(game, "A", workspace_of(game));
    GameObject& b = named_part(game, "B", workspace_of(game));
    join<RigidConstraint>(game, b.id(), a.id(), a.id());
    const std::vector<InstanceId> made = duplicate(game, {a.id()});
    auto* copy = as<RigidConstraint>(game, game.first_child(made[0]));
    REQUIRE(copy != nullptr);
    REQUIRE(copy->pv_instance2_id() == made[0]);
    REQUIRE(copy->pv_instance1_id() == 0);
    REQUIRE(copy->pv_instance1_ref().guid().empty());
}
