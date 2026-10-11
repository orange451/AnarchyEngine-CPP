// Bone: an Attachment under a skinned GameObject that poses the bone it is
// named for. Its Offset is added after the bone's own local transform, and its
// Transform is where that bone is in the world.

#include "skinning_support.hpp"

#include "Assemblies.hpp"
#include "Bone.hpp"
#include "Brush.hpp"
#include "ChangeHistoryService.hpp"
#include "Constraint.hpp"
#include "LuaApi.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "Skeleton.hpp"
#include "brush/BrushGeometry.hpp"

using namespace skinning_test;

namespace {

engine_core::Bone& add_bone(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name) {
    engine_core::Bone& made = game.create<engine_core::Bone>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), parent);
    return made;
}

Vec3 origin(const Matrix4& value) { return engine_core::matrix4_position(value); }

}  // namespace

TEST_CASE("BONE1 an unposed Bone is where its bone rests", "[bone]") {
    Rig rig;
    REQUIRE(engine_core::lua_class_inherits("Bone", "Attachment"));
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f));
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    REQUIRE(hand.bone_index() == 1);
    REQUIRE(near_matrix(hand.transform(), engine_core::matrix4_translation(3.f, 2.f, 0.f)));
}

TEST_CASE("BONE2 a Bone's Offset turns its bone and every bone below, and adds to theirs", "[bone]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    engine_core::Bone& root = add_bone(rig.game, object.id(), "Root");
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    REQUIRE_FALSE(root.set_offset(spin_z()));
    REQUIRE(near_vec(origin(hand.transform()), Vec3{-2.f, 0.f, 0.f}));
    REQUIRE_FALSE(hand.set_offset(engine_core::matrix4_translation(1.f, 0.f, 0.f)));
    REQUIRE(near_vec(origin(hand.transform()), Vec3{-2.f, 1.f, 0.f}));
}

TEST_CASE("BONE3 a Bone follows its GameObject's Transform and Scale", "[bone]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    const Matrix4 turn = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, kQuarter / 2.0);
    object.set_transform(engine_core::matrix4_multiply(engine_core::matrix4_translation(5.f, 0.f, 0.f), turn));
    REQUIRE_FALSE(object.set_scale(2.0));
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    REQUIRE_FALSE(hand.set_offset(engine_core::matrix4_translation(1.f, 0.f, 0.f)));
    // (1, 2, 0) in the arm, doubled, then turned and moved.
    const Vec3 expected = engine_core::matrix4_point(object.transform(), Vec3{2.f, 4.f, 0.f});
    REQUIRE(near_vec(origin(hand.transform()), expected));
}

TEST_CASE("BONE4 writing a Bone's Transform sets the Offset that puts it there, and undoes", "[bone]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(0.f, 0.f, 4.f));
    engine_core::Bone& root = add_bone(rig.game, object.id(), "Root");
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    REQUIRE_FALSE(root.set_offset(spin_z()));
    const Matrix4 target = engine_core::matrix4_multiply(engine_core::matrix4_translation(1.f, 2.f, 3.f), spin_z());
    begin_step(rig.game, "Move Bone");
    REQUIRE_FALSE(hand.set_transform(target));
    end_step(rig.game);
    REQUIRE(near_matrix(hand.transform(), target));
    rig.game.history().undo();
    REQUIRE(near_matrix(hand.offset(), engine_core::matrix4_identity()));
    REQUIRE(near_vec(origin(hand.transform()), Vec3{-2.f, 0.f, 4.f}));
}

TEST_CASE("BONE5 a Bone with no bone of its name, or a second one, is a plain Attachment", "[bone]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    engine_core::Bone& nope = add_bone(rig.game, object.id(), "Nope");
    REQUIRE(nope.bone_index() == -1);
    REQUIRE_FALSE(nope.set_offset(engine_core::matrix4_translation(1.f, 0.f, 0.f)));
    REQUIRE(near_vec(origin(nope.transform()), Vec3{1.f, 1.f, 0.f}));
    engine_core::Bone& first = add_bone(rig.game, object.id(), "Hand");
    engine_core::Bone& second = add_bone(rig.game, object.id(), "Hand");
    REQUIRE(first.bone_index() == 1);
    REQUIRE(second.bone_index() == -1);
    REQUIRE(near_vec(origin(second.transform()), Vec3{0.f, 1.f, 0.f}));
    // Under something that is not a GameObject, it is an Attachment too.
    engine_core::Bone& loose = add_bone(rig.game, workspace_of(rig.game), "Hand");
    REQUIRE(loose.bone_index() == -1);
}

TEST_CASE("BONE6 a GameObject's pose is made again only when what poses it changes", "[bone]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    const auto first = object.pose();
    REQUIRE(first != nullptr);
    REQUIRE(first->owners[1] == hand.id());
    REQUIRE(object.pose() == first);
    REQUIRE_FALSE(hand.set_offset(spin_z()));
    const auto second = object.pose();
    REQUIRE(second != first);
    rig.game.set_name(hand.id(), "Root");
    REQUIRE(object.pose()->owners[0] == hand.id());
    REQUIRE(create_part(rig.game).pose() == nullptr);
}

TEST_CASE("BONE7 scripts list a GameObject's bones, and GetBone makes a Bone the first time it is asked", "[bone]") {
    ScriptRig rig;
    TempDir dir;
    write_mesh(dir / "meshes" / "arm.amesh", arm(2));
    rig.game.set_resources_root(dir.path);
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    rig.game.set_parent(mesh.id(), rig.game.service("Meshes"));
    REQUIRE_FALSE(mesh.set_path("meshes/arm.amesh"));
    engine_core::Prefab& prefab = rig.game.create<engine_core::Prefab>();
    rig.game.set_parent(prefab.id(), rig.game.service("Prefabs"));
    engine_core::Model& model = rig.game.create<engine_core::Model>();
    rig.game.set_parent(model.id(), prefab.id());
    REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, id_slot(mesh.id())));
    engine_core::GameObject& object = create_part(rig.game);
    rig.game.set_name(object.id(), "Arm");
    REQUIRE_FALSE(object.set_prefab(id_slot(prefab.id())));
    add_script(rig.game, "Pose", R"(
        local arm = workspace.Arm
        local names = arm:GetBoneNames()
        _G.names = #names == 2 and names[1] == "Root" and names[2] == "Hand"
        _G.none = #arm:GetChildren() == 0
        local hand = arm:GetBone("Hand")
        _G.added = hand ~= nil and hand.ClassName == "Bone" and hand.Name == "Hand" and hand.Parent == arm
        _G.twice = arm:GetBone("Hand") == hand and #arm:GetChildren() == 1
        _G.unknown = arm:GetBone("Nope") == nil and #arm:GetChildren() == 1
        _G.plain = #Instance.new("GameObject", workspace):GetBoneNames() == 0
        hand.Offset = Matrix4.new() + Vector3.new(1, 0, 0)
        _G.moved = hand.Transform.Position == Vector3.new(1, 2, 0)
    )");
    rig.game.start_simulation();
    rig.frames(2);
    INFO(rig.runtime.last_error());
    for (const char* name : {"names", "none", "added", "twice", "unknown", "plain", "moved"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    rig.game.stop_simulation();
}

#include "SnapshotPump.hpp"

namespace {

// rig's game drawn into a pump, a frame at a time.
struct Drawn {
    engine_core::SnapshotPump pump;

    Drawn() { pump.reserve(engine_core::DataModel::kMaxInstances); }

    void frame(engine_core::DataModel& game) {
        pump.prepare_copy(game);
        pump.publish();
    }
};

}  // namespace

TEST_CASE("BONE8 the render snapshot carries a skinned GameObject's pose", "[bone]") {
    Rig rig;
    Drawn drawn;
    engine_core::GameObject& object = rig.arm_object();
    engine_core::GameObject& plain = create_part(rig.game);
    drawn.frame(rig.game);
    const engine_core::VisualInstance* row = drawn.pump.find(object.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->pose != nullptr);
    REQUIRE(row->pose->palette.size() == 24);
    REQUIRE(drawn.pump.find(plain.id())->pose == nullptr);
    const auto first = row->pose;

    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    REQUIRE_FALSE(hand.set_offset(spin_z()));
    drawn.frame(rig.game);
    row = drawn.pump.find(object.id());
    REQUIRE(row->pose != first);
    REQUIRE(row->pose->owners[1] == hand.id());
}

TEST_CASE("BONE9 a Prefab's Models whose Mesh has another skeleton draw unposed", "[bone]") {
    Rig rig;
    Drawn drawn;
    engine_core::Mesh& box = rig.mesh("meshes/box.amesh");
    engine_core::Mesh& arm_mesh = rig.mesh("meshes/arm.amesh");
    engine_core::Prefab& prefab = rig.prefab({box.id(), arm_mesh.id()});
    engine_core::GameObject& object = create_part(rig.game);
    REQUIRE_FALSE(object.set_prefab(id_slot(prefab.id())));
    drawn.frame(rig.game);
    const engine_core::VisualInstance* row = drawn.pump.find(object.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->pose != nullptr);
    const engine_core::VisualPrefab& drawn_prefab = drawn.pump.front().prefabs[row->prefab];
    REQUIRE(drawn_prefab.meshes.size() == 2);
    REQUIRE_FALSE(drawn_prefab.meshes[0].skinned);
    REQUIRE(drawn_prefab.meshes[1].skinned);
}

TEST_CASE("BONE10 a RigidConstraint to a Bone holds its sub on the posed bone, and follows the pose", "[bone][assembly]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f));
    REQUIRE_FALSE(object.set_scale(2.0));
    // Anchored, so the arm is its assembly's root and the held part the sub.
    engine_core::PhysicsObject& body = rig.game.create<engine_core::PhysicsObject>();
    rig.game.set_parent(body.id(), object.id());
    body.set_anchored(true);
    engine_core::Bone& root = add_bone(rig.game, object.id(), "Root");
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    engine_core::GameObject& held = create_part(rig.game);
    engine_core::RigidConstraint& grip = rig.game.create<engine_core::RigidConstraint>();
    rig.game.set_parent(grip.id(), workspace_of(rig.game));
    REQUIRE_FALSE(grip.set_pv_instance1(id_slot(hand.id())));
    REQUIRE_FALSE(grip.set_pv_instance2(id_slot(held.id())));
    // The hand rests 2 up the arm, doubled by its Scale: the pivot has no scale.
    REQUIRE(near_vec(origin(held.transform()), Vec3{3.f, 4.f, 0.f}));
    REQUIRE(near_matrix(held.transform(), engine_core::matrix4_orthonormalize(hand.transform())));
    // Turning the bone above it moves the hand, and the held part with it.
    REQUIRE_FALSE(root.set_offset(spin_z()));
    REQUIRE(near_vec(origin(held.transform()), Vec3{-1.f, 0.f, 0.f}));
    REQUIRE(near_matrix(held.transform(), engine_core::matrix4_orthonormalize(hand.transform())));
}

TEST_CASE("BONE11 a hand holds what it holds however heavy, and the two stay apart", "[bone][assembly]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f));
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    // Anchored and heavy: as two Parts, it would be the root and the arm would move.
    engine_core::GameObject& held = create_part(rig.game);
    held.set_transform(engine_core::matrix4_translation(0.f, 9.f, 0.f));
    engine_core::PhysicsObject& body = rig.game.create<engine_core::PhysicsObject>();
    rig.game.set_parent(body.id(), held.id());
    body.set_anchored(true);
    REQUIRE_FALSE(body.set_mass(1e6));
    engine_core::RigidConstraint& grip = rig.game.create<engine_core::RigidConstraint>();
    rig.game.set_parent(grip.id(), workspace_of(rig.game));
    REQUIRE_FALSE(grip.set_pv_instance1(id_slot(held.id())));
    REQUIRE_FALSE(grip.set_pv_instance2(id_slot(hand.id())));
    REQUIRE(near_vec(origin(object.transform()), Vec3{3.f, 0.f, 0.f}));
    REQUIRE(near_vec(origin(held.transform()), Vec3{3.f, 2.f, 0.f}));
    engine_core::Assemblies& assemblies = rig.game.assemblies();
    assemblies.refresh(rig.game);
    REQUIRE(assemblies.active(grip.id()));
    REQUIRE(assemblies.root_of(rig.game, held.id()) == body.id());
    REQUIRE(assemblies.root_of(rig.game, object.id()) == object.id());
}

TEST_CASE("BONE12 in play a hand carries what it holds, kinematic, wherever the pose and its GameObject take it",
          "[bone][assembly][play]") {
    Rig rig;
    engine_core::PhysicsWorld physics;
    constexpr double kStep = 1.0 / 240.0;
    const auto at = [](float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); };
    const auto near3 = [](Vec3 a, Vec3 b, float tolerance) {
        return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
    };
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(at(0.f, 1.f, 0.f));
    engine_core::PhysicsObject& stand = rig.game.create<engine_core::PhysicsObject>();
    rig.game.set_parent(stand.id(), object.id());
    stand.set_anchored(true);
    engine_core::Bone& root = add_bone(rig.game, object.id(), "Root");
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    // An unanchored box: let go of, it would fall.
    engine_core::PhysicsObject& box = rig.game.create<engine_core::PhysicsObject>();
    REQUIRE_FALSE(box.set_transform(at(5.f, 0.f, 0.f)));
    REQUIRE_FALSE(box.set_size(Vec3{0.5f, 0.5f, 0.5f}));
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    engine_core::RigidConstraint& grip = rig.game.create<engine_core::RigidConstraint>();
    rig.game.set_parent(grip.id(), workspace_of(rig.game));
    REQUIRE_FALSE(grip.set_pv_instance1(id_slot(hand.id())));
    REQUIRE_FALSE(grip.set_pv_instance2(id_slot(box.id())));
    REQUIRE(near_vec(origin(box.transform()), Vec3{0.f, 3.f, 0.f}));

    rig.game.capture_place();
    rig.game.start_simulation();
    for (int i = 0; i < 240; ++i) {
        physics.step(rig.game, kStep);
    }
    // A second on: still in the hand, and its body with it.
    REQUIRE(near3(origin(box.transform()), Vec3{0.f, 3.f, 0.f}, 1e-3f));
    REQUIRE(physics.body_position(box.id()).has_value());
    REQUIRE(near3(*physics.body_position(box.id()), Vec3{0.f, 3.f, 0.f}, 1e-3f));

    // The pose changes, as an animation's does: the box is drawn on the hand
    // after the next step, and its body gets there by moving.
    REQUIRE_FALSE(root.set_offset(spin_z()));
    physics.step(rig.game, kStep);
    REQUIRE(near3(origin(box.transform()), Vec3{-2.f, 1.f, 0.f}, 1e-3f));
    for (int i = 0; i < 4; ++i) {
        physics.step(rig.game, kStep);
    }
    REQUIRE(near3(*physics.body_position(box.id()), Vec3{-2.f, 1.f, 0.f}, 1e-2f));

    // Its GameObject moves: the hand, and what it holds, go with it.
    object.set_transform(at(4.f, 1.f, 0.f));
    physics.step(rig.game, kStep);
    REQUIRE(near3(origin(box.transform()), Vec3{2.f, 1.f, 0.f}, 1e-3f));
    for (int i = 0; i < 4; ++i) {
        physics.step(rig.game, kStep);
    }
    REQUIRE(near3(*physics.body_position(box.id()), Vec3{2.f, 1.f, 0.f}, 1e-2f));
}

TEST_CASE("BONE13 a hand brings a welded gun whole when enabled, and keeps it whole as the pose moves",
          "[bone][assembly]") {
    Rig rig;
    engine_core::GameObject& object = rig.arm_object();
    object.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f));
    engine_core::Bone& root = add_bone(rig.game, object.id(), "Root");
    engine_core::Bone& hand = add_bone(rig.game, object.id(), "Hand");
    // Two Brushes welded into a gun: a grip, and a larger barrel that is its root.
    engine_core::Brush& grip = rig.game.create<engine_core::Brush>();
    rig.game.set_parent(grip.id(), workspace_of(rig.game));
    REQUIRE_FALSE(grip.set_transform(engine_core::matrix4_translation(0.f, 9.f, 0.f)));
    engine_core::Brush& barrel = rig.game.create<engine_core::Brush>();
    rig.game.set_parent(barrel.id(), workspace_of(rig.game));
    REQUIRE_FALSE(barrel.set_faces(engine_core::brush::make_box({8.0, 2.0, 2.0})));
    REQUIRE_FALSE(barrel.set_transform(engine_core::matrix4_translation(2.f, 11.f, 0.f)));
    engine_core::WeldConstraint& weld = rig.game.create<engine_core::WeldConstraint>();
    REQUIRE_FALSE(weld.set_pv_instance1(id_slot(barrel.id())));
    REQUIRE_FALSE(weld.set_pv_instance2(id_slot(grip.id())));
    rig.game.set_parent(weld.id(), barrel.id());
    engine_core::RigidConstraint& hold = rig.game.create<engine_core::RigidConstraint>();
    rig.game.set_parent(hold.id(), workspace_of(rig.game));
    REQUIRE_FALSE(hold.set_enabled(false));
    REQUIRE_FALSE(hold.set_pv_instance1(id_slot(grip.id())));
    REQUIRE_FALSE(hold.set_pv_instance2(id_slot(hand.id())));
    const Matrix4 offset =
        engine_core::matrix4_multiply(engine_core::matrix4_inverse(grip.transform()), barrel.transform());
    REQUIRE_FALSE(hold.set_enabled(true));
    REQUIRE(near_matrix(grip.transform(), engine_core::matrix4_orthonormalize(hand.transform())));
    REQUIRE(near_matrix(barrel.transform(), engine_core::matrix4_multiply(grip.transform(), offset)));
    // Posing the arm moves the whole gun.
    REQUIRE_FALSE(root.set_offset(spin_z()));
    REQUIRE(near_matrix(grip.transform(), engine_core::matrix4_orthonormalize(hand.transform())));
    REQUIRE(near_matrix(barrel.transform(), engine_core::matrix4_multiply(grip.transform(), offset)));
    // The grip dragged alone snaps back by itself; the barrel stays where it is.
    const Matrix4 barrel_before = barrel.transform();
    REQUIRE_FALSE(grip.set_transform(engine_core::matrix4_translation(50.f, 50.f, 50.f)));
    REQUIRE(near_matrix(grip.transform(), engine_core::matrix4_orthonormalize(hand.transform())));
    REQUIRE(near_matrix(barrel.transform(), barrel_before));
}
