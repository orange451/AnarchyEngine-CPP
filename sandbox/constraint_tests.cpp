// WeldConstraint and RigidConstraint: their two PVInstance ends, Enabled, and
// how they save, load, undo, and are found in Workspace.

#include "support.hpp"

#include "Attachment.hpp"
#include "Constraint.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "PhysicsObject.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using engine_core::Constraint;
using engine_core::InstanceId;
using engine_core::RigidConstraint;
using engine_core::WeldConstraint;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot nil_slot() { return engine_core::LuaSlot{}; }

bool listed(const engine_core::DataModel& game, InstanceId id) {
    std::vector<InstanceId> out;
    game.constraints(out);
    return std::find(out.begin(), out.end(), id) != out.end();
}

}  // namespace

TEST_CASE("CN1 a new constraint has no ends and is Enabled", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    WeldConstraint& weld = game.create<WeldConstraint>();
    REQUIRE(weld.pv_instance1_id() == 0);
    REQUIRE(weld.pv_instance2_id() == 0);
    REQUIRE(weld.pv_instance1().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(weld.enabled());
    REQUIRE_FALSE(weld.rigid());
    REQUIRE(weld.constraint());
    REQUIRE(engine_core::lua_class_inherits("WeldConstraint", "Constraint"));
    REQUIRE(engine_core::lua_class_inherits("RigidConstraint", "Constraint"));
    REQUIRE(engine_core::lua_class_inherits("Constraint", "Instance"));
    RigidConstraint& rigid = game.create<RigidConstraint>();
    REQUIRE(rigid.rigid());
    REQUIRE(std::string(weld.class_name()) == "WeldConstraint");
    REQUIRE(std::string(rigid.class_name()) == "RigidConstraint");
}

TEST_CASE("CN2 an end takes any PVInstance and refuses anything else", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    engine_core::Attachment& point = game.create<engine_core::Attachment>();
    game.set_parent(point.id(), part.id());
    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    game.set_parent(body.id(), part.id());
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));

    WeldConstraint& weld = game.create<WeldConstraint>();
    for (const InstanceId target : {part.id(), point.id(), body.id()}) {
        REQUIRE_FALSE(weld.set_pv_instance1(instance_slot(target)));
        REQUIRE(weld.pv_instance1_id() == target);
    }
    const auto refused = weld.set_pv_instance1(instance_slot(folder.id()));
    REQUIRE(refused);
    REQUIRE(*refused == "PVInstance1 must be a PVInstance");
    REQUIRE(weld.pv_instance1_id() == body.id());
    REQUIRE_FALSE(weld.set_pv_instance1(nil_slot()));
    REQUIRE(weld.pv_instance1_id() == 0);
}

TEST_CASE("CN3 both ends may name the same instance", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    WeldConstraint& weld = game.create<WeldConstraint>();
    REQUIRE_FALSE(weld.set_pv_instance1(instance_slot(part.id())));
    REQUIRE_FALSE(weld.set_pv_instance2(instance_slot(part.id())));
    REQUIRE(weld.pv_instance2_id() == part.id());
}

TEST_CASE("CN4 scripts make constraints and set their ends", "[assembly][constraint]") {
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Part");
    add_script(rig.game, "Main", R"(
        local w = Instance.new("WeldConstraint", workspace)
        w.Name = "W"
        w.PVInstance1 = workspace.Part
        w.Enabled = false
        local ok = pcall(function() Instance.new("Constraint") end)
        local f = Instance.new("Folder")
        f.Name = if ok then "made" else "refused"
        f.Parent = workspace
        local r = Instance.new("RigidConstraint", workspace)
        r.Name = "R"
    )");
    rig.game.start_simulation();
    rig.frames(2);
    const InstanceId w = rig.game.find_first_child(workspace_of(rig.game), "W");
    auto* weld = dynamic_cast<WeldConstraint*>(rig.game.instance(w));
    REQUIRE(weld != nullptr);
    REQUIRE(weld->pv_instance1_id() == part.id());
    REQUIRE_FALSE(weld->enabled());
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "refused") != 0);
    REQUIRE(dynamic_cast<RigidConstraint*>(
                rig.game.instance(rig.game.find_first_child(workspace_of(rig.game), "R"))) != nullptr);
}

TEST_CASE("CN5 constraints lists only those in Workspace", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    WeldConstraint& inside = game.create<WeldConstraint>();
    game.set_parent(inside.id(), workspace_of(game));
    WeldConstraint& stored = game.create<WeldConstraint>();
    game.set_parent(stored.id(), game.scene_service("Storage"));
    RigidConstraint& gone = game.create<RigidConstraint>();
    game.set_parent(gone.id(), workspace_of(game));
    const InstanceId gone_id = gone.id();
    game.destroy(gone_id);
    REQUIRE(listed(game, inside.id()));
    REQUIRE_FALSE(listed(game, stored.id()));
    REQUIRE_FALSE(listed(game, gone_id));
    game.set_parent(stored.id(), workspace_of(game));
    REQUIRE(listed(game, stored.id()));
}

TEST_CASE("CN6 an end write undoes", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& a = create_part(game);
    engine_core::GameObject& b = create_part(game);
    RigidConstraint& rigid = game.create<RigidConstraint>();
    game.set_parent(rigid.id(), workspace_of(game));
    REQUIRE_FALSE(rigid.set_pv_instance2(instance_slot(a.id())));
    begin_step(game, "Set PVInstance2");
    REQUIRE_FALSE(rigid.set_pv_instance2(instance_slot(b.id())));
    end_step(game);
    REQUIRE(rigid.pv_instance2_id() == b.id());
    game.history().undo();
    REQUIRE(rigid.pv_instance2_id() == a.id());
    game.history().redo();
    REQUIRE(rigid.pv_instance2_id() == b.id());
}

TEST_CASE("CN7 a project saves and loads both classes, their ends by GUID, and Enabled", "[assembly][constraint]") {
    SimRole role;
    TempDir dir;
    std::string a_guid;
    std::string b_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::GameObject& a = create_part(game);
        game.set_name(a.id(), "A");
        engine_core::GameObject& b = create_part(game);
        game.set_name(b.id(), "B");
        a_guid = game.guid(a.id());
        b_guid = game.guid(b.id());
        WeldConstraint& weld = game.create<WeldConstraint>();
        game.set_name(weld.id(), "Weld");
        REQUIRE_FALSE(weld.set_pv_instance1(instance_slot(a.id())));
        REQUIRE_FALSE(weld.set_pv_instance2(instance_slot(b.id())));
        REQUIRE_FALSE(weld.set_enabled(false));
        game.set_parent(weld.id(), workspace_of(game));
        RigidConstraint& rigid = game.create<RigidConstraint>();
        game.set_name(rigid.id(), "Rigid");
        REQUIRE_FALSE(rigid.set_pv_instance1(instance_slot(b.id())));
        game.set_parent(rigid.id(), workspace_of(game));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const InstanceId ws = workspace_of(game);
    auto* weld = dynamic_cast<WeldConstraint*>(game.instance(game.find_first_child(ws, "Weld")));
    auto* rigid = dynamic_cast<RigidConstraint*>(game.instance(game.find_first_child(ws, "Rigid")));
    REQUIRE(weld != nullptr);
    REQUIRE(rigid != nullptr);
    REQUIRE(game.guid(weld->pv_instance1_id()) == a_guid);
    REQUIRE(game.guid(weld->pv_instance2_id()) == b_guid);
    REQUIRE_FALSE(weld->enabled());
    REQUIRE(game.guid(rigid->pv_instance1_id()) == b_guid);
    REQUIRE(rigid->pv_instance2_id() == 0);
    REQUIRE(rigid->enabled());
}

TEST_CASE("CN8 a destroyed end reads nil and heals on undo", "[assembly][constraint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    const InstanceId part_id = part.id();
    WeldConstraint& weld = game.create<WeldConstraint>();
    game.set_parent(weld.id(), workspace_of(game));
    REQUIRE_FALSE(weld.set_pv_instance1(instance_slot(part_id)));
    const std::string guid = weld.pv_instance1_ref().guid();
    begin_step(game, "Delete");
    game.destroy(part_id);
    end_step(game);
    REQUIRE(weld.pv_instance1_id() == 0);
    REQUIRE(weld.pv_instance1_ref().guid() == guid);
    game.history().undo();
    REQUIRE(weld.pv_instance1_id() == part_id);
}
