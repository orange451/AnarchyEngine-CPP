// The animation core: easing, clips ready to sample, and the step that turns
// playing tracks into one change from rest per skeleton bone.

#include "support.hpp"

#include "Animation.hpp"
#include "aanim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

namespace aanim = anarchy::aanim;
using engine_core::BonePose;
using engine_core::TrackEvent;
using engine_core::TrackState;

constexpr float kPi = 3.14159265f;

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

aanim::Pose pose_at(const char* bone, float x, float degrees_z, float weight = 1.f) {
    aanim::Pose out;
    out.bone = bone;
    out.position[0] = x;
    const float half = degrees_z * kPi / 360.f;
    out.rotation[2] = std::sin(half);
    out.rotation[3] = std::cos(half);
    out.weight = weight;
    return out;
}

// A at x 0, 2, 4 and B turned 0, 90, 180 degrees about Z, at t 0, 1, 2.
aanim::Data walk() {
    aanim::Data data;
    data.name = "Walk";
    const char* names[] = {"Start", "Middle", "End"};
    for (int k = 0; k < 3; ++k) {
        aanim::Keyframe keyframe;
        keyframe.time = static_cast<float>(k);
        keyframe.name = names[k];
        keyframe.poses = {pose_at("A", 2.f * k, 0.f), pose_at("B", 0.f, 90.f * k)};
        data.keyframes.push_back(keyframe);
    }
    return data;
}

// One keyframe: the pose held for as long as it plays.
aanim::Data hold(std::vector<aanim::Pose> poses) {
    aanim::Data data;
    data.name = "Hold";
    aanim::Keyframe keyframe;
    keyframe.poses = std::move(poses);
    data.keyframes.push_back(keyframe);
    return data;
}

TrackState track_of(const aanim::Data& data, std::uint32_t id = 1) {
    TrackState track;
    track.id = id;
    track.clip = engine_core::make_clip(data);
    track.looped = track.clip->looped;
    track.bone_map.resize(track.clip->bones.size());
    // Bones A, B, ... map to skeleton bones 0, 1, ... by name.
    for (std::size_t b = 0; b < track.clip->bones.size(); ++b) {
        track.bone_map[b] = track.clip->bones[b][0] - 'A';
    }
    return track;
}

struct Stepper {
    std::vector<TrackState> tracks;
    std::vector<BonePose> out;
    std::vector<bool> touched;
    std::vector<TrackEvent> events;

    void step(float dt) {
        events.clear();
        engine_core::step_animations(tracks, dt, 2, out, touched, events);
    }
    float turned_z(int bone) const { return 2.f * std::atan2(out[bone].rotation.z, out[bone].rotation.w) * 180.f / kPi; }
    int count(TrackEvent::Kind kind) const {
        int found = 0;
        for (const TrackEvent& event : events) {
            found += event.kind == kind ? 1 : 0;
        }
        return found;
    }
    bool reached(int index) const {
        for (const TrackEvent& event : events) {
            if (event.kind == TrackEvent::Kind::KeyframeReached && event.index == index) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE("AC1 every easing runs from 0 to 1, along Roblox's curves", "[animation]") {
    for (std::uint8_t style = 0; style < aanim::kEasingStyleCount; ++style) {
        for (std::uint8_t direction = 0; direction < aanim::kEasingDirectionCount; ++direction) {
            INFO(int(style) << " " << int(direction));
            const auto s = static_cast<aanim::EasingStyle>(style);
            const auto d = static_cast<aanim::EasingDirection>(direction);
            REQUIRE(near(engine_core::ease(s, d, 0.f), 0.f));
            REQUIRE(near(engine_core::ease(s, d, 1.f), 1.f));
        }
    }
    using S = aanim::EasingStyle;
    using D = aanim::EasingDirection;
    REQUIRE(near(engine_core::ease(S::Linear, D::In, 0.3f), 0.3f));
    REQUIRE(near(engine_core::ease(S::Quad, D::In, 0.5f), 0.25f));
    REQUIRE(near(engine_core::ease(S::Quad, D::Out, 0.5f), 0.75f));
    REQUIRE(near(engine_core::ease(S::Quad, D::InOut, 0.25f), 0.125f));
    REQUIRE(near(engine_core::ease(S::Cubic, D::In, 0.5f), 0.125f));
    REQUIRE(near(engine_core::ease(S::Constant, D::In, 0.99f), 0.f));
    REQUIRE(near(engine_core::ease(S::Bounce, D::Out, 0.5f), 0.765625f));
    REQUIRE(near(engine_core::ease(S::Sine, D::Out, 0.5f), std::sin(kPi / 4.f)));
}

TEST_CASE("AC2 a track samples between its keyframes", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(walk()));
    engine_core::track_play(stepper.tracks[0], 0.f, 1.f, 1.f);
    stepper.step(0.5f);
    REQUIRE(near(stepper.out[0].position.x, 1.f));
    REQUIRE(near(stepper.turned_z(1), 45.f, 1e-2f));
    REQUIRE(stepper.touched[0]);
    REQUIRE(stepper.reached(0));
    stepper.step(1.f);
    REQUIRE(near(stepper.out[0].position.x, 3.f));
    REQUIRE(stepper.reached(1));
}

TEST_CASE("AC3 a looped track wraps, an unlooped one clamps, either way along", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(walk()));
    TrackState& track = stepper.tracks[0];
    engine_core::track_play(track, 0.f, 1.f, 1.f);
    track.looped = true;
    track.time = 1.9f;
    stepper.step(0.6f);
    REQUIRE(near(track.time, 0.5f));
    // The wrap passes the last keyframe.
    REQUIRE(stepper.reached(2));
    REQUIRE(near(stepper.out[0].position.x, 1.f));

    track.looped = false;
    track.time = 1.9f;
    stepper.step(0.6f);
    REQUIRE(near(track.time, 2.f));
    REQUIRE(near(stepper.out[0].position.x, 4.f));

    track.looped = true;
    track.speed = -1.f;
    track.time = 0.25f;
    stepper.step(0.5f);
    REQUIRE(near(track.time, 1.75f));
    track.looped = false;
    track.time = 0.25f;
    stepper.step(0.5f);
    REQUIRE(near(track.time, 0.f));

    // A time written far past the end comes back into it.
    track.looped = true;
    track.speed = 1.f;
    track.time = 5.f;
    stepper.step(0.f);
    REQUIRE(near(track.time, 1.f));
}

TEST_CASE("AC4 Play fades in from nothing; Stop fades out, then stops and says so once", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(walk()));
    TrackState& track = stepper.tracks[0];
    engine_core::track_play(track, 0.2f, 1.f, 1.f);
    REQUIRE(track.playing);
    REQUIRE(track.weight_current == 0.f);
    stepper.step(0.1f);
    REQUIRE(near(track.weight_current, 0.5f));
    // Half its weight: half way from rest to its pose.
    REQUIRE(near(stepper.out[0].position.x, 0.1f));
    stepper.step(0.1f);
    REQUIRE(near(track.weight_current, 1.f));
    engine_core::track_stop(track, 0.2f);
    stepper.step(0.1f);
    REQUIRE(track.playing);
    REQUIRE(stepper.count(TrackEvent::Kind::Stopped) == 0);
    stepper.step(0.1f);
    REQUIRE(near(track.weight_current, 0.f));
    REQUIRE_FALSE(track.playing);
    REQUIRE(stepper.count(TrackEvent::Kind::Stopped) == 1);
    stepper.step(0.1f);
    REQUIRE(stepper.count(TrackEvent::Kind::Stopped) == 0);
    // Stopped at once, it still says so on the next step.
    engine_core::track_play(track, 0.f, 1.f, 1.f);
    engine_core::track_stop(track, 0.f);
    REQUIRE_FALSE(track.playing);
    stepper.step(0.f);
    REQUIRE(stepper.count(TrackEvent::Kind::Stopped) == 1);
}

TEST_CASE("AC5 tracks on one bone share it by weight; less than 1 in all leans toward rest", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(hold({pose_at("A", 0.f, 0.f)}), 1));
    stepper.tracks.push_back(track_of(hold({pose_at("A", 2.f, 0.f)}), 2));
    engine_core::track_play(stepper.tracks[0], 0.f, 1.f, 1.f);
    engine_core::track_play(stepper.tracks[1], 0.f, 1.f, 1.f);
    stepper.step(0.f);
    REQUIRE(near(stepper.out[0].position.x, 1.f));
    // A crossfade: the weights always add to 1, so the bone never sags toward rest.
    engine_core::track_adjust_weight(stepper.tracks[0], 0.25f, 0.f);
    engine_core::track_adjust_weight(stepper.tracks[1], 0.75f, 0.f);
    stepper.step(0.f);
    REQUIRE(near(stepper.out[0].position.x, 1.5f));
    // One track alone at half weight: half way there from rest.
    stepper.tracks.pop_back();
    engine_core::track_adjust_weight(stepper.tracks[0], 0.5f, 0.f);
    stepper.tracks[0].clip = engine_core::make_clip(hold({pose_at("A", 4.f, 0.f)}));
    stepper.step(0.f);
    REQUIRE(near(stepper.out[0].position.x, 2.f));
}

TEST_CASE("AC6 a pose of weight 0 masks its bone out of a clip played over another", "[animation]") {
    Stepper stepper;
    // A walk that moves A and B, and a wave that moves B only.
    stepper.tracks.push_back(track_of(hold({pose_at("A", 2.f, 0.f), pose_at("B", 0.f, 0.f)}), 1));
    stepper.tracks.push_back(track_of(hold({pose_at("A", 8.f, 0.f, 0.f), pose_at("B", 0.f, 90.f)}), 2));
    engine_core::track_play(stepper.tracks[0], 0.f, 1.f, 1.f);
    engine_core::track_play(stepper.tracks[1], 0.f, 1.f, 1.f);
    stepper.step(0.f);
    REQUIRE(near(stepper.out[0].position.x, 2.f));
    REQUIRE(near(stepper.turned_z(1), 45.f, 1e-2f));
}

TEST_CASE("AC7 a weight 0 track keeps time and does nothing else", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(walk()));
    engine_core::track_play(stepper.tracks[0], 0.f, 1.f, 0.f);
    stepper.step(0.5f);
    REQUIRE(near(stepper.tracks[0].time, 0.5f));
    REQUIRE(stepper.events.empty());
    REQUIRE_FALSE(stepper.touched[0]);
    REQUIRE(near(stepper.out[0].position.x, 0.f));
}

TEST_CASE("AC8 a bone the skeleton lacks is skipped, and a bone nothing moves is at rest", "[animation]") {
    Stepper stepper;
    stepper.tracks.push_back(track_of(walk()));
    stepper.tracks[0].bone_map[1] = -1;
    engine_core::track_play(stepper.tracks[0], 0.f, 1.f, 1.f);
    stepper.step(0.5f);
    REQUIRE(stepper.touched[0]);
    REQUIRE_FALSE(stepper.touched[1]);
    REQUIRE(near(stepper.out[1].rotation.w, 1.f));
    REQUIRE(near(stepper.out[1].scale.y, 1.f));
    const engine_core::Matrix4 delta =
        engine_core::bone_delta_matrix(engine_core::Vec3{1.f, 2.f, 3.f}, engine_core::Quat{}, engine_core::Vec3{2.f, 2.f, 2.f});
    REQUIRE(near(delta.m[0], 2.f));
    REQUIRE(near(delta.m[13], 2.f));
}
