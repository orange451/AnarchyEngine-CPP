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
