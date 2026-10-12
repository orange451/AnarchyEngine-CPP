// Assemblies: which PVInstances constraints join, which constraints are
// Active, and which member is each assembly's root.

#include "support.hpp"

#include "Assemblies.hpp"
#include "Attachment.hpp"
#include "Brush.hpp"
#include "Constraint.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "Terrain.hpp"
#include "brush/BrushGeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::Assemblies;
using engine_core::Constraint;
using engine_core::DataModel;
using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::PhysicsObject;
using engine_core::RigidConstraint;
using engine_core::Vec3;
using engine_core::WeldConstraint;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A GameObject in Workspace with a PhysicsObject under it that moves it.
struct Part {
    GameObject* object = nullptr;
    PhysicsObject* body = nullptr;
};

Part add_part(DataModel& game, double mass = PhysicsObject::kDefaultMass, bool anchored = false) {
    Part part;
    part.object = &create_part(game);
    part.body = &game.create<PhysicsObject>();
    REQUIRE_FALSE(part.body->set_mass(mass));
    part.body->set_anchored(anchored);
    game.set_parent(part.body->id(), part.object->id());
    return part;
}

template <class T>
T& join(DataModel& game, InstanceId a, InstanceId b, InstanceId parent = 0) {
    T& constraint = game.create<T>();
    REQUIRE_FALSE(constraint.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(b)));
    game.set_parent(constraint.id(), parent != 0 ? parent : workspace_of(game));
    return constraint;
}

WeldConstraint& weld(DataModel& game, InstanceId a, InstanceId b) { return join<WeldConstraint>(game, a, b); }
RigidConstraint& rigid(DataModel& game, InstanceId a, InstanceId b) { return join<RigidConstraint>(game, a, b); }

Assemblies& fresh(DataModel& game) {
    game.assemblies().refresh(game);
    return game.assemblies();
}

InstanceId root(DataModel& game, InstanceId id) { return fresh(game).root_of(game, id); }

bool active(DataModel& game, const Constraint& constraint) { return fresh(game).active(constraint.id()); }

// Runs body once for each creation order and constraint direction; make
// returns the instance that must be root.
template <class Make>
void four_ways(Make make) {
    for (int order = 0; order < 2; ++order) {
        for (int direction = 0; direction < 2; ++direction) {
            SimRole role;
            engine_core::Game game;
            make(game, order == 1, direction == 1);
        }
    }
}

}  // namespace

TEST_CASE("AS1 two welded parts are one group, and both halves of a part are one node", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    WeldConstraint& w = weld(game, a.object->id(), b.body->id());
    Assemblies& graph = fresh(game);
    REQUIRE(graph.active(w.id()));
    REQUIRE(graph.groups().size() == 1);
    REQUIRE(graph.groups()[0].count == 2);
    REQUIRE(graph.node_of(a.object->id()) == a.body->id());
    REQUIRE(graph.node_of(a.body->id()) == a.body->id());
    REQUIRE(graph.node_of(b.object->id()) == b.body->id());
    const InstanceId r = graph.root_of(game, a.object->id());
    REQUIRE((r == a.body->id() || r == b.body->id()));
    for (const InstanceId id : {a.object->id(), a.body->id(), b.object->id(), b.body->id()}) {
        REQUIRE(graph.root_of(game, id) == r);
        REQUIRE((graph.flags(id) & Assemblies::kMember) != 0);
        REQUIRE((graph.flags(id) & Assemblies::kLinked) != 0);
    }
}

TEST_CASE("AS2 anchored is root over a heavier unanchored part", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part heavy;
        Part anchored;
        if (swap_order) {
            anchored = add_part(game, 1.0, true);
            heavy = add_part(game, 500.0);
        } else {
            heavy = add_part(game, 500.0);
            anchored = add_part(game, 1.0, true);
        }
        if (swap_direction) {
            weld(game, anchored.object->id(), heavy.object->id());
        } else {
            weld(game, heavy.object->id(), anchored.object->id());
        }
        REQUIRE(root(game, heavy.object->id()) == anchored.body->id());
        REQUIRE(fresh(game).groups()[0].anchored);
    });
}

TEST_CASE("AS3 a PlayerController is root over a heavier PhysicsObject", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part crate;
        engine_core::PlayerController* player = nullptr;
        auto make_player = [&] {
            player = &game.create<engine_core::PlayerController>();
            game.set_parent(player->id(), workspace_of(game));
        };
        if (swap_order) {
            make_player();
            crate = add_part(game, 500.0);
        } else {
            crate = add_part(game, 500.0);
            make_player();
        }
        if (swap_direction) {
            weld(game, player->id(), crate.object->id());
        } else {
            weld(game, crate.object->id(), player->id());
        }
        REQUIRE(root(game, crate.object->id()) == player->id());
    });
}

TEST_CASE("AS4 a physics node is root over a plain GameObject", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part part;
        GameObject* plain = nullptr;
        if (swap_order) {
            plain = &create_part(game);
            part = add_part(game, 0.5);
        } else {
            part = add_part(game, 0.5);
            plain = &create_part(game);
        }
        if (swap_direction) {
            weld(game, plain->id(), part.object->id());
        } else {
            weld(game, part.object->id(), plain->id());
        }
        REQUIRE(root(game, plain->id()) == part.body->id());
        REQUIRE(fresh(game).groups()[0].has_physics);
    });
}

TEST_CASE("AS5 the higher Mass is root", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part light;
        Part heavy;
        if (swap_order) {
            heavy = add_part(game, 30.0);
            light = add_part(game, 10.0);
        } else {
            light = add_part(game, 10.0);
            heavy = add_part(game, 30.0);
        }
        if (swap_direction) {
            weld(game, heavy.object->id(), light.object->id());
        } else {
            weld(game, light.object->id(), heavy.object->id());
        }
        REQUIRE(root(game, light.object->id()) == heavy.body->id());
    });
}

TEST_CASE("AS6 with equal Mass the lower PhysicsBase GUID is root", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part first = add_part(game);
        Part second = add_part(game);
        Part& low = swap_order ? second : first;
        Part& high = swap_order ? first : second;
        game.set_guid(low.body->id(), "aaaa-body");
        game.set_guid(high.body->id(), "bbbb-body");
        // The GameObjects' GUIDs sort the other way: they are not compared.
        game.set_guid(low.object->id(), "zzzz-object");
        game.set_guid(high.object->id(), "0000-object");
        if (swap_direction) {
            weld(game, high.object->id(), low.object->id());
        } else {
            weld(game, low.object->id(), high.object->id());
        }
        REQUIRE(root(game, high.object->id()) == low.body->id());
    });
}

TEST_CASE("AS7 a weld to an Attachment joins the part it is on, which is root, never the Attachment",
          "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, 10.0);
    Part b = add_part(game, 90.0);
    engine_core::Attachment& point = game.create<engine_core::Attachment>();
    game.set_parent(point.id(), b.object->id());
    weld(game, a.object->id(), point.id());
    Assemblies& graph = fresh(game);
    REQUIRE(graph.node_of(point.id()) == b.body->id());
    REQUIRE(graph.root_of(game, point.id()) == b.body->id());
    REQUIRE(graph.root_of(game, a.object->id()) == b.body->id());
}

TEST_CASE("AS8 an Attachment with no PVInstance holder is no end", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    engine_core::Attachment& in_folder = game.create<engine_core::Attachment>();
    game.set_parent(in_folder.id(), folder.id());
    engine_core::Attachment& loose = game.create<engine_core::Attachment>();
    game.set_parent(loose.id(), workspace_of(game));
    WeldConstraint& w1 = weld(game, a.object->id(), in_folder.id());
    WeldConstraint& w2 = weld(game, loose.id(), a.object->id());
    REQUIRE_FALSE(active(game, w1));
    REQUIRE_FALSE(active(game, w2));
    REQUIRE(root(game, in_folder.id()) == 0);
    REQUIRE(root(game, loose.id()) == 0);
    REQUIRE(fresh(game).groups().empty());
}

TEST_CASE("AS9 a constraint missing an end, disabled, or outside Workspace is inactive", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    WeldConstraint& w = weld(game, a.object->id(), b.object->id());
    REQUIRE(active(game, w));
    REQUIRE_FALSE(w.set_pv_instance2(engine_core::LuaSlot{}));
    REQUIRE_FALSE(active(game, w));
    REQUIRE_FALSE(w.set_pv_instance2(instance_slot(b.object->id())));
    REQUIRE_FALSE(w.set_enabled(false));
    REQUIRE_FALSE(active(game, w));
    REQUIRE_FALSE(w.set_enabled(true));
    game.set_parent(w.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(active(game, w));
    game.set_parent(w.id(), workspace_of(game));
    game.set_parent(b.object->id(), game.scene_service("Storage"));
    REQUIRE_FALSE(active(game, w));
    game.set_parent(b.object->id(), workspace_of(game));
    REQUIRE(active(game, w));
}

TEST_CASE("AS10 a Terrain end is inactive", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    engine_core::Terrain& terrain = game.create<engine_core::Terrain>();
    game.set_parent(terrain.id(), workspace_of(game));
    WeldConstraint& w = weld(game, a.object->id(), terrain.id());
    REQUIRE_FALSE(active(game, w));
    REQUIRE(root(game, terrain.id()) == 0);
}

TEST_CASE("AS11 rigid edges form a tree: one that closes a loop is inactive, and follow order puts parents first",
          "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, 100.0);
    Part b = add_part(game);
    Part c = add_part(game);
    Part d = add_part(game);
    // Two edges onto c make a tree, not a loop: both are active.
    RigidConstraint& a_c = rigid(game, a.object->id(), c.object->id());
    game.set_guid(a_c.id(), "r1");
    RigidConstraint& b_c = rigid(game, b.object->id(), c.object->id());
    game.set_guid(b_c.id(), "r2");
    REQUIRE(active(game, a_c));
    REQUIRE(active(game, b_c));
    // A second edge between b and d closes a loop: the higher GUID is inactive.
    RigidConstraint& d_b = rigid(game, d.object->id(), b.object->id());
    game.set_guid(d_b.id(), "r3");
    RigidConstraint& b_d = rigid(game, b.object->id(), d.object->id());
    game.set_guid(b_d.id(), "r4");
    REQUIRE(active(game, d_b));
    REQUIRE_FALSE(active(game, b_d));
    // Away from the root a: a to c, c to b, b to d, in that order.
    const auto& edges = fresh(game).rigid_edges();
    REQUIRE(edges.size() == 3);
    REQUIRE(edges[0].constraint == a_c.id());
    REQUIRE(edges[0].parent_node == a.body->id());
    REQUIRE(edges[0].sub_node == c.body->id());
    REQUIRE(edges[0].parent_end == 1);
    REQUIRE(edges[1].constraint == b_c.id());
    REQUIRE(edges[1].parent_node == c.body->id());
    REQUIRE(edges[1].sub_node == b.body->id());
    REQUIRE(edges[1].parent_end == 2);
    REQUIRE(edges[2].constraint == d_b.id());
    REQUIRE(edges[2].parent_node == b.body->id());
    REQUIRE(edges[2].sub_node == d.body->id());
    REQUIRE(edges[2].parent_end == 2);

    // A chain whose parent edge has the higher GUID still follows parent first.
    engine_core::Game chain_game;
    Part x = add_part(chain_game, 100.0);
    Part y = add_part(chain_game);
    Part z = add_part(chain_game);
    RigidConstraint& yz = rigid(chain_game, y.object->id(), z.object->id());
    chain_game.set_guid(yz.id(), "a-sub-edge");
    RigidConstraint& xy = rigid(chain_game, x.object->id(), y.object->id());
    chain_game.set_guid(xy.id(), "z-parent-edge");
    const auto& chain = fresh(chain_game).rigid_edges();
    REQUIRE(chain.size() == 2);
    REQUIRE(chain[0].constraint == xy.id());
    REQUIRE(chain[1].constraint == yz.id());
    REQUIRE(chain[0].sub_node == y.body->id());
    REQUIRE(chain[1].parent_node == y.body->id());
}

TEST_CASE("AS12 a constraint whose two ends are one node is inactive", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    engine_core::Attachment& point = game.create<engine_core::Attachment>();
    game.set_parent(point.id(), a.object->id());
    WeldConstraint& own_body = weld(game, a.object->id(), a.body->id());
    RigidConstraint& own_point = rigid(game, point.id(), a.object->id());
    REQUIRE_FALSE(active(game, own_body));
    REQUIRE_FALSE(active(game, own_point));
    REQUIRE(fresh(game).groups().empty());
    REQUIRE(fresh(game).rigid_edges().empty());
}

TEST_CASE("AS13 a PhysicsBase that lost its GameObject belongs to the winner's node", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    // A second body under the same GameObject: the first in tree order drives it.
    PhysicsObject& loser = game.create<PhysicsObject>();
    game.set_parent(loser.id(), a.object->id());
    Part b = add_part(game);
    weld(game, loser.id(), b.object->id());
    Assemblies& graph = fresh(game);
    REQUIRE(graph.claimant_of(a.object->id()) == a.body->id());
    REQUIRE(graph.node_of(loser.id()) == a.body->id());
    REQUIRE(graph.groups().size() == 1);
    REQUIRE(graph.group_of(a.body->id()) == graph.group_of(b.body->id()));
}

TEST_CASE("AS14 AssemblyRoot reads the same from Lua, in the editor and in play", "[assembly][graph]") {
    ScriptRig rig;
    DataModel& game = rig.game;
    Part lone = add_part(game);
    game.set_name(lone.object->id(), "Lone");
    game.set_name(lone.body->id(), "LoneBody");
    Part a = add_part(game, 10.0);
    game.set_name(a.object->id(), "A");
    Part b = add_part(game, 90.0);
    game.set_name(b.object->id(), "B");
    game.set_name(b.body->id(), "BBody");
    weld(game, a.object->id(), b.object->id());
    GameObject& plain = create_part(game);
    game.set_name(plain.id(), "Plain");
    GameObject& stored = game.create_game_object();
    game.set_name(stored.id(), "Stored");
    game.set_parent(stored.id(), game.scene_service("Storage"));

    REQUIRE(root(game, lone.object->id()) == lone.body->id());
    REQUIRE(root(game, a.object->id()) == b.body->id());
    REQUIRE(root(game, plain.id()) == plain.id());
    REQUIRE(root(game, stored.id()) == 0);

    add_script(game, "Main", R"(
        local function name(x) return if x then x.Name else "nil" end
        local out = Instance.new("Folder")
        out.Name = table.concat({
            name(workspace.Lone.AssemblyRoot), name(workspace.A.AssemblyRoot),
            name(workspace.Plain.AssemblyRoot), name(game:GetService("Storage").Stored.AssemblyRoot),
        }, ",")
        out.Parent = workspace
    )");
    game.start_simulation();
    rig.frames(2);
    REQUIRE(game.find_first_child(workspace_of(game), "LoneBody,BBody,Plain,nil") != 0);
    REQUIRE(root(game, lone.object->id()) == lone.body->id());
    REQUIRE(root(game, a.object->id()) == b.body->id());
    REQUIRE(root(game, plain.id()) == plain.id());
}

TEST_CASE("AS15 in play, a rigid edge that welds already join is inactive", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    weld(game, a.object->id(), b.object->id());
    RigidConstraint& r = rigid(game, a.object->id(), b.object->id());
    REQUIRE(active(game, r));
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(active(game, r));
    game.stop_simulation();
    REQUIRE(active(game, r));
}

TEST_CASE("AS16 renames and unrelated bodies leave the assembly revision alone", "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    weld(game, a.object->id(), b.object->id());
    Assemblies& graph = fresh(game);
    const std::uint64_t before = graph.revision();
    game.set_name(a.object->id(), "Renamed");
    Part bullet = add_part(game);
    bullet.object->set_transform(engine_core::matrix4_translation(3.f, 4.f, 5.f));
    game.set_parent(bullet.object->id(), game.scene_service("Storage"));
    game.set_parent(bullet.object->id(), workspace_of(game));
    REQUIRE(graph.revision() == before);
}

TEST_CASE("AS17 constraint edits and changes to linked instances move the assembly revision",
          "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    Part c = add_part(game);
    WeldConstraint& w = weld(game, a.object->id(), b.object->id());
    Assemblies& graph = fresh(game);
    auto moves = [&](auto&& change) {
        fresh(game);
        const std::uint64_t before = graph.revision();
        change();
        return graph.revision() != before;
    };
    REQUIRE(moves([&] { REQUIRE_FALSE(w.set_pv_instance1(instance_slot(c.object->id()))); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(w.set_enabled(false)); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(w.set_enabled(true)); }));
    REQUIRE(moves([&] { game.set_parent(w.id(), c.object->id()); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(b.body->set_mass(77.0)); }));
    REQUIRE(moves([&] { b.body->set_anchored(true); }));
    REQUIRE(moves([&] {
        PhysicsObject& extra = game.create<PhysicsObject>();
        game.set_parent(extra.id(), c.object->id());
    }));
    REQUIRE(moves([&] { game.destroy(b.object->id()); }));
}

TEST_CASE("AS18 a suspended scope still moves the revision, and a refresh sees the edit", "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    WeldConstraint& w = weld(game, a.object->id(), b.object->id());
    Assemblies& graph = fresh(game);
    REQUIRE(graph.active(w.id()));
    const std::uint64_t before = graph.revision();
    {
        engine_core::AssemblySuspend suspend(game);
        REQUIRE(graph.suspended());
        REQUIRE_FALSE(w.set_enabled(false));
        REQUIRE(graph.revision() != before);
        REQUIRE_FALSE(fresh(game).active(w.id()));
    }
    REQUIRE_FALSE(graph.suspended());
}

TEST_CASE("AS19 a dangling end is not searched for again until a GUID may have come back",
          "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    for (int i = 0; i < 200; ++i) {
        WeldConstraint& w = game.create<WeldConstraint>();
        engine_core::LuaSlot missing;
        missing.kind = engine_core::LuaSlot::Kind::Nil;  // a GUID as load writes one
        missing.text = "gone-" + std::to_string(i);
        REQUIRE_FALSE(w.set_pv_instance2(missing));
        REQUIRE_FALSE(w.set_pv_instance1(instance_slot(a.object->id())));
        game.set_parent(w.id(), workspace_of(game));
    }
    Assemblies& graph = fresh(game);
    const std::size_t scans = graph.find_guid_calls();
    for (int i = 0; i < 1000; ++i) {
        graph.bump();
        graph.refresh(game);
    }
    REQUIRE(graph.find_guid_calls() == scans);

    // A GUID that arrives later is found.
    Part b = add_part(game);
    game.set_guid(b.object->id(), "gone-7");
    REQUIRE(fresh(game).groups().size() == 1);
    REQUIRE(fresh(game).group_of(b.body->id()) == fresh(game).group_of(a.body->id()));
}

TEST_CASE("AS20 Play and Stop alone change which rigid edges are active", "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    weld(game, a.object->id(), b.object->id());
    RigidConstraint& r = rigid(game, a.object->id(), b.object->id());
    Assemblies& graph = fresh(game);
    REQUIRE(graph.active(r.id()));
    game.capture_place();
    game.start_simulation();
    graph.refresh(game);
    REQUIRE_FALSE(graph.active(r.id()));
    game.stop_simulation();
    graph.refresh(game);
    REQUIRE(graph.active(r.id()));
}

TEST_CASE("AS21 undoing the delete of a rigid sub makes its constraint active again", "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    RigidConstraint& r = rigid(game, a.object->id(), b.object->id());
    REQUIRE(active(game, r));
    begin_step(game, "Delete");
    game.destroy(b.object->id());
    end_step(game);
    REQUIRE_FALSE(active(game, r));
    game.history().undo();
    REQUIRE(active(game, r));
}

TEST_CASE("AS22 with equal Mass the larger body is root, over a lower GUID", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part first = add_part(game);
        Part second = add_part(game);
        Part& small = swap_order ? second : first;
        Part& large = swap_order ? first : second;
        // The GUIDs alone would make the small one root.
        game.set_guid(small.body->id(), "aaaa-body");
        game.set_guid(large.body->id(), "bbbb-body");
        REQUIRE_FALSE(large.body->set_size(Vec3{4.f, 2.f, 4.f}));
        if (swap_direction) {
            weld(game, large.object->id(), small.object->id());
        } else {
            weld(game, small.object->id(), large.object->id());
        }
        REQUIRE(root(game, small.object->id()) == large.body->id());
    });
}

TEST_CASE("AS23 with equal Mass the larger Brush is root", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        engine_core::Brush& first = game.create<engine_core::Brush>();
        game.set_parent(first.id(), workspace_of(game));
        engine_core::Brush& second = game.create<engine_core::Brush>();
        game.set_parent(second.id(), workspace_of(game));
        engine_core::Brush& small = swap_order ? second : first;
        engine_core::Brush& large = swap_order ? first : second;
        game.set_guid(small.id(), "aaaa-brush");
        game.set_guid(large.id(), "bbbb-brush");
        REQUIRE_FALSE(large.set_faces(engine_core::brush::make_box({8.0, 8.0, 8.0})));
        if (swap_direction) {
            rigid(game, large.id(), small.id());
        } else {
            rigid(game, small.id(), large.id());
        }
        REQUIRE(root(game, small.id()) == large.id());
    });
}

TEST_CASE("AS24 a GameObject's Scale counts toward its body's size", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part first = add_part(game);
        Part second = add_part(game);
        Part& small = swap_order ? second : first;
        Part& large = swap_order ? first : second;
        game.set_guid(small.body->id(), "aaaa-body");
        game.set_guid(large.body->id(), "bbbb-body");
        REQUIRE_FALSE(large.object->set_scale(3.0));
        if (swap_direction) {
            weld(game, large.object->id(), small.object->id());
        } else {
            weld(game, small.object->id(), large.object->id());
        }
        REQUIRE(root(game, small.object->id()) == large.body->id());
    });
}

TEST_CASE("AS25 a linked body's size moves the assembly revision, and an unlinked one's does not",
          "[assembly][revision]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game);
    Part b = add_part(game);
    Part loose = add_part(game);
    weld(game, a.object->id(), b.object->id());
    engine_core::Brush& brush = game.create<engine_core::Brush>();
    game.set_parent(brush.id(), workspace_of(game));
    weld(game, a.object->id(), brush.id());
    Assemblies& graph = fresh(game);
    auto moves = [&](auto&& change) {
        fresh(game);
        const std::uint64_t before = graph.revision();
        change();
        return graph.revision() != before;
    };
    REQUIRE(moves([&] { REQUIRE_FALSE(b.body->set_size(Vec3{2.f, 2.f, 2.f})); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(b.body->set_shape(static_cast<int>(PhysicsObject::Shape::Sphere))); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(b.object->set_scale(2.0)); }));
    REQUIRE(moves([&] { REQUIRE_FALSE(brush.set_faces(engine_core::brush::make_box({6.0, 6.0, 6.0}))); }));
    REQUIRE_FALSE(moves([&] { REQUIRE_FALSE(loose.body->set_size(Vec3{3.f, 3.f, 3.f})); }));
    REQUIRE_FALSE(moves([&] { REQUIRE_FALSE(loose.object->set_scale(2.0)); }));
}

TEST_CASE("AS26 a RigidConstraint points away from the root, whichever end names it", "[assembly][graph]") {
    four_ways([](engine_core::Game& game, bool swap_order, bool swap_direction) {
        Part light;
        Part heavy;
        if (swap_order) {
            heavy = add_part(game, 30.0);
            light = add_part(game, 10.0);
        } else {
            light = add_part(game, 10.0);
            heavy = add_part(game, 30.0);
        }
        if (swap_direction) {
            rigid(game, heavy.object->id(), light.object->id());
        } else {
            rigid(game, light.object->id(), heavy.object->id());
        }
        const auto& edges = fresh(game).rigid_edges();
        REQUIRE(edges.size() == 1);
        REQUIRE(edges[0].parent_node == heavy.body->id());
        REQUIRE(edges[0].sub_node == light.body->id());
        REQUIRE(edges[0].parent_end == (swap_direction ? 1 : 2));
    });
}

TEST_CASE("AS27 a rigid edge on a welded member points away from the root's side", "[assembly][graph]") {
    SimRole role;
    engine_core::Game game;
    Part root_part = add_part(game, 100.0);
    Part member = add_part(game);
    Part far = add_part(game, 90.0);
    weld(game, root_part.object->id(), member.object->id());
    rigid(game, far.object->id(), member.object->id());
    const auto& edges = fresh(game).rigid_edges();
    REQUIRE(edges.size() == 1);
    REQUIRE(edges[0].parent_node == member.body->id());
    REQUIRE(edges[0].sub_node == far.body->id());
    REQUIRE(edges[0].parent_end == 2);
}
