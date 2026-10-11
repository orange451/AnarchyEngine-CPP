#pragma once

// Playing animation clips, with no DataModel: easing, a clip ready to sample,
// a track's state, and the step that blends playing tracks into one change
// from rest per skeleton bone. After the user's GrooveAnimator (1.0.9), whose
// step this is.

#include "Matrix4.hpp"
#include "aanim.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

// From a to b by t along the shorter arc, unit length.
Quat quat_slerp(Quat a, Quat b, float t);
// T(position) * R(rotation) * S(scale).
Matrix4 bone_delta_matrix(Vec3 position, Quat rotation, Vec3 scale);
// Roblox's easing curves: t from 0 to 1 eased, 0 at 0 and 1 at 1. Constant
// holds 0 until t reaches 1.
float ease(anarchy::aanim::EasingStyle style, anarchy::aanim::EasingDirection direction, float t);

// A bone's change from its rest local transform.
struct BonePose {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.f, 1.f, 1.f};
};

// A clip ready to sample, shared and immutable: its bones' names once each,
// and each keyframe's poses by clip bone. next pairs each of a keyframe's
// poses with the same bone's in the keyframe after it (or itself, when that
// keyframe has none, or it is the last), self with itself: indices into the
// keyframe's own poses and the next one's.
struct Clip {
    struct KeyPose {
        std::uint16_t bone = 0;
        BonePose pose;
        anarchy::aanim::EasingStyle style = anarchy::aanim::EasingStyle::Linear;
        anarchy::aanim::EasingDirection direction = anarchy::aanim::EasingDirection::In;
        float weight = 1.f;
    };
    struct Keyframe {
        float time = 0.f;
        std::string name;
        std::vector<KeyPose> poses;
        // (pose in this keyframe, pose in the next keyframe, or -1 for this one's again).
        std::vector<std::pair<int, int>> next;
    };
    std::string name;
    bool looped = false;
    // The last keyframe's time.
    float length = 0.f;
    std::vector<std::string> bones;
    std::vector<Keyframe> keyframes;
};

std::shared_ptr<const Clip> make_clip(const anarchy::aanim::Data& data);

// What a step heard: a track reaching a keyframe (its name and index), or a
// track's Stop finishing.
struct TrackEvent {
    enum class Kind { KeyframeReached, Stopped };
    Kind kind = Kind::KeyframeReached;
    std::uint32_t track = 0;
    std::string name;
    int index = 0;
};

// One playing (or stopped) clip. bone_map takes each clip bone to a
// skeleton bone, or -1 for one the skeleton lacks.
struct TrackState {
    std::uint32_t id = 0;
    std::shared_ptr<const Clip> clip;
    std::vector<int> bone_map;
    bool looped = false;
    bool playing = false;
    float speed = 1.f;
    float time = 0.f;
    float weight_current = 0.f;
    float weight_target = 1.f;
    // A fade of the weight and of the speed: from, to, how far in, and how
    // long (below 0: none). A Stop's fade ends the track when it gets to 0.
    float w_from = 0.f, w_to = 0.f, w_t = 0.f, w_dur = -1.f;
    bool stop_on_fade = false;
    float s_from = 0.f, s_to = 0.f, s_t = 0.f, s_dur = -1.f;
    // A Stop finished, for the next step to say so.
    bool stopped_pending = false;
    // How many keyframes come before the time, as last found, and the
    // keyframe last reached: -1 for none yet.
    int key_lo = -1;
    int keyframe_index = -1;
};

// Plays from where the track is: a track not playing fades in from weight 0.
void track_play(TrackState& track, float fade, float speed, float weight);
// Fades to weight 0, then stops (Stopped on the next step). A fade of 0 or
// less stops it at once.
void track_stop(TrackState& track, float fade);
void track_adjust_weight(TrackState& track, float weight, float fade);
void track_adjust_speed(TrackState& track, float speed, float fade);

// What a step gathers per bone, kept between steps by its caller so a step
// allocates only while a rig or a blend grows.
struct AnimationScratch {
    std::vector<std::vector<std::pair<BonePose, float>>> parts;
};

// Steps tracks by dt and writes one change from rest per skeleton bone into
// out (bone_count of them; rest for bones no track moves), with touched
// saying which a track moved, and events what was heard, in order. An
// unlooped track that reaches its end (its start, played backward) holds
// there and stops with the default fade, Stopped coming when the fade does.
//
// A bone's contributions are each track's weight times its pose's weight.
// Their weighted average is the bone's pose when they add to 1 or more; when
// they add to less, it leans toward rest by what is missing. So a lone track
// fades in from rest, a crossfade never sags, and a pose of weight 0 leaves
// its bone to the other tracks.
void step_animations(std::vector<TrackState>& tracks, float dt, std::size_t bone_count, std::vector<BonePose>& out,
                     std::vector<bool>& touched, std::vector<TrackEvent>& events,
                     AnimationScratch* scratch = nullptr);

}  // namespace engine_core
