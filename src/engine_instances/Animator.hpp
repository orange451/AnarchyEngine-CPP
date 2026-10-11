#pragma once

#include "Animation.hpp"
#include "DataModel.hpp"
#include "Events.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {

// What an Animator's last step made of its tracks: one change from rest per
// bone of its GameObject's skeleton (the one whose signature is skeleton),
// for GameObject::pose to put under the Bone Offsets. revision is unique to
// each step.
struct AnimatedPose {
    std::uint64_t revision = 0;
    std::uint64_t skeleton = 0;
    std::vector<Matrix4> deltas;
};

// Plays Animation clips on the skinned GameObject it is under, as tracks: the
// runtime half of Animator:LoadAnimation. Tracks are never saved or undone; a
// Stop drops them, as do a reuse and a destroy.
//
// AutoStep  boolean  true. Saved. Play mode steps it once a frame, after
//                    PreAnimation (step_animators). Edit mode never does;
//                    step, StepAnimations to scripts, works in both.
class Animator : public DataModel {
public:
    Animator(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "Animator"; }
    bool animator() const override { return true; }

    bool auto_step() const { return auto_step_; }
    // SimulationThread.
    std::optional<std::string> set_auto_step(bool value);

    // A new, stopped track of the Animation animation: its id. 0 when
    // animation is not a live Animation. SimulationThread.
    std::uint32_t load(InstanceId animation);
    // The track, or null when it is gone: destroyed, or dropped by a Stop.
    TrackState* track(std::uint32_t id);
    // The Animation a track plays, as loaded.
    InstanceId track_animation(std::uint32_t id) const;
    void destroy_track(std::uint32_t id);
    // The playing tracks' ids, in the order they were made.
    std::vector<std::uint32_t> playing();

    // Steps every track by dt seconds, stores the pose they make, and fires
    // their events. With no skinned GameObject above it, time still runs.
    // SimulationThread.
    void step(double dt);
    // The last step's pose, or null before one, after a Stop, or with no
    // skinned GameObject above it. Shared and immutable. Under the DataModel lock.
    std::shared_ptr<const AnimatedPose> animated() const;

    // A track's KeyframeReached (name, index from 1) or Stopped signal, made
    // the first time it is asked for and gone with the track.
    Signal& track_signal(std::uint32_t id, TrackEvent::Kind kind);

protected:
    void on_reuse() override;

private:
    // What the Animator keeps beside each track, slot for slot with tracks_.
    struct Slot {
        InstanceId animation = 0;
        // The skeleton signature bone_map was matched to; all ones before the first match.
        std::uint64_t matched = ~std::uint64_t{0};
    };

    // Drops every track when a Stop came since they were made.
    void drop_if_stale();
    void drop_all();
    void release_signals(std::uint32_t id);

    bool auto_step_ = true;
    // The tracks, in the order made, and what is kept beside each.
    std::vector<TrackState> tracks_;
    std::vector<Slot> slots_;
    std::uint32_t next_track_ = 1;
    std::uint32_t generation_ = 0;
    std::map<std::pair<std::uint32_t, int>, std::unique_ptr<Signal>> signals_;
    // step's scratch, kept so a step allocates only while it grows.
    std::vector<BonePose> poses_;
    std::vector<bool> touched_;
    AnimationScratch scratch_;
    std::vector<TrackEvent> events_;

    mutable std::mutex animated_mutex_;
    std::shared_ptr<const AnimatedPose> animated_;
    std::uint32_t animated_generation_ = 0;
};

}  // namespace engine_core
