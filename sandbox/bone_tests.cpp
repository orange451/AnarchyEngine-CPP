// Bone: an Attachment under a skinned GameObject that poses the bone it is
// named for. Its Offset is added after the bone's own local transform, and its
// Transform is where that bone is in the world.

#include "skinning_support.hpp"

#include "Bone.hpp"
#include "ChangeHistoryService.hpp"
#include "LuaApi.hpp"
#include "Skeleton.hpp"

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

TEST_CASE("BONE7 scripts list a GameObject's bones, find its Bones, and add them", "[bone]") {
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
        _G.none = arm:GetBone("Hand") == nil
        local hand = arm:AddBone("Hand")
        _G.added = hand.ClassName == "Bone" and hand.Name == "Hand" and hand.Parent == arm
            and arm:GetBone("Hand") == hand
        local ok, why = pcall(function() arm:AddBone("Hand") end)
        _G.twice = not ok and string.find(why, "already has a Bone for Hand", 1, true) ~= nil
        ok, why = pcall(function() arm:AddBone("Nope") end)
        _G.unknown = not ok and string.find(why, "no bone named Nope", 1, true) ~= nil
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
