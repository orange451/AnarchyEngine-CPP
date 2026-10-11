// The Animator: tracks of Animation clips on a skinned GameObject, stepped by
// Play while AutoStep is on, or by StepAnimations at any time, under the
// GameObject's Bone Offsets.

#include "skinning_support.hpp"

#include "Animation.hpp"
#include "AnimatorStep.hpp"
#include "Animator.hpp"
#include "Bone.hpp"
#include "ChangeHistoryService.hpp"
#include "aanim.hpp"

using namespace skinning_test;

namespace {

namespace aanim = anarchy::aanim;

// Hand slides 2 units along X over a second, then holds; Tail is a bone no mesh has.
aanim::Data slide(bool with_tail = false) {
    aanim::Data data;
    data.name = "Slide";
    for (int k = 0; k < 2; ++k) {
        aanim::Keyframe keyframe;
        keyframe.time = static_cast<float>(k);
        aanim::Pose hand;
        hand.bone = "Hand";
        hand.position[0] = 2.f * static_cast<float>(k);
        keyframe.poses.push_back(hand);
        if (with_tail) {
            aanim::Pose tail;
            tail.bone = "Tail";
            tail.position[1] = 5.f;
            keyframe.poses.push_back(tail);
        }
        data.keyframes.push_back(keyframe);
    }
    return data;
}

void write_clip(const std::filesystem::path& file, const aanim::Data& data) {
    std::filesystem::create_directories(file.parent_path());
    const std::vector<std::byte> bytes = aanim::write(data);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// The arm, an Animator under it, and the slide clip as an Animation.
struct AnimRig {
    Rig rig;
    engine_core::GameObject* arm = nullptr;
    engine_core::Animator* animator = nullptr;
    engine_core::Animation* clip = nullptr;
    engine_core::Bone* hand = nullptr;

    explicit AnimRig(bool with_tail = false) {
        write_clip(rig.dir / "animations" / "slide.aanim", slide(with_tail));
        arm = &rig.arm_object();
        animator = &rig.game.create<engine_core::Animator>();
        rig.game.set_parent(animator->id(), arm->id());
        clip = &rig.game.create<engine_core::Animation>();
        rig.game.set_parent(clip->id(), rig.game.service("Animations"));
        REQUIRE_FALSE(clip->set_path("animations/slide.aanim"));
        hand = &rig.game.create<engine_core::Bone>();
        rig.game.set_name(hand->id(), "Hand");
        rig.game.set_parent(hand->id(), arm->id());
        rig.game.history().reset_waypoints();
    }

    std::uint32_t play() {
        const std::uint32_t id = animator->load(clip->id());
        REQUIRE(id != 0);
        engine_core::track_play(*animator->track(id), 0.f, 1.f, 1.f);
        return id;
    }

    Vec3 hand_at() const { return engine_core::matrix4_position(hand->transform()); }
};

}  // namespace

TEST_CASE("ANI1 a playing track moves its bone, and the Bone's Offset adds on top", "[animator]") {
    AnimRig anim;
    REQUIRE(engine_core::lua_class_inherits("Animator", "Instance"));
    REQUIRE(anim.animator->auto_step());
    REQUIRE(anim.animator->load(anim.arm->id()) == 0);
    anim.play();
    REQUIRE(near_vec(anim.hand_at(), Vec3{0.f, 2.f, 0.f}));
    anim.animator->step(0.5);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 0.f}));
    REQUIRE_FALSE(anim.hand->set_offset(engine_core::matrix4_translation(0.f, 0.f, 1.f)));
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 1.f}));
    // Writing the Bone's Transform still solves for the Offset, over the animation.
    REQUIRE_FALSE(anim.hand->set_transform(engine_core::matrix4_translation(1.f, 3.f, 0.f)));
    REQUIRE(near_vec(engine_core::matrix4_position(anim.hand->offset()), Vec3{0.f, 1.f, 0.f}));
}

TEST_CASE("ANI2 Play steps an Animator while AutoStep is on; edit mode never does; step always can", "[animator]") {
    AnimRig anim;
    anim.play();
    engine_core::step_animators(anim.rig.game, 0.5);
    REQUIRE(near_vec(anim.hand_at(), Vec3{0.f, 2.f, 0.f}));
    // A track made in edit mode lasts until Play starts; one made in Play, until Stop.
    anim.rig.game.start_simulation();
    anim.play();
    engine_core::step_animators(anim.rig.game, 0.5);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 0.f}));
    REQUIRE_FALSE(anim.animator->set_auto_step(false));
    engine_core::step_animators(anim.rig.game, 0.25);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 0.f}));
    anim.animator->step(0.25);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.5f, 2.f, 0.f}));
    anim.rig.game.stop_simulation();
}

TEST_CASE("ANI3 Stop drops every track, and the pose is back to its Offsets", "[animator]") {
    AnimRig anim;
    REQUIRE_FALSE(anim.hand->set_offset(engine_core::matrix4_translation(0.f, 0.f, 1.f)));
    anim.rig.game.history().reset_waypoints();
    anim.rig.game.start_simulation();
    const std::uint32_t id = anim.play();
    anim.animator->step(0.5);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 1.f}));
    anim.rig.game.stop_simulation();
    REQUIRE(anim.animator->track(id) == nullptr);
    REQUIRE(anim.animator->playing().empty());
    REQUIRE(near_vec(anim.hand_at(), Vec3{0.f, 2.f, 1.f}));
}

TEST_CASE("ANI4 AutoStep is a saved property, and undoes", "[animator]") {
    AnimRig anim;
    begin_step(anim.rig.game, "AutoStep");
    REQUIRE_FALSE(anim.animator->set_auto_step(false));
    end_step(anim.rig.game);
    REQUIRE_FALSE(anim.animator->auto_step());
    anim.rig.game.history().undo();
    REQUIRE(anim.animator->auto_step());
}

TEST_CASE("ANI5 a clip bone the mesh lacks is left out, and the rest still moves", "[animator]") {
    AnimRig anim(true);
    anim.play();
    anim.animator->step(0.5);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.f, 2.f, 0.f}));
}

TEST_CASE("ANI6 a new skeleton under a playing track is matched by name at the next step", "[animator]") {
    AnimRig anim;
    anim.play();
    anim.animator->step(0.5);
    // The same arm with a Finger: Hand is still bone 1, and the track finds it again.
    write_mesh(anim.rig.dir / "meshes" / "arm3.amesh", arm(3));
    engine_core::Prefab& longer = anim.rig.prefab({anim.rig.mesh("meshes/arm3.amesh").id()});
    REQUIRE_FALSE(anim.arm->set_prefab(id_slot(longer.id())));
    anim.animator->step(0.25);
    REQUIRE(near_vec(anim.hand_at(), Vec3{1.5f, 2.f, 0.f}));
    // No skinned parent at all: time still runs, nothing breaks.
    REQUIRE_FALSE(anim.arm->set_prefab(engine_core::LuaSlot()));
    anim.animator->step(0.25);
    REQUIRE(anim.animator->animated() == nullptr);
}
