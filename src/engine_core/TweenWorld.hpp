#pragma once

// TweenService's tweens, with no Lua in them
// (docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md).
// A tween moves properties of one instance, or one plain value, from where
// they were when it started to a goal, along an easing curve. The script
// runtime owns one TweenWorld and steps it once a play frame.

#include "Events.hpp"
#include "LuaApi.hpp"
#include "aanim.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;

struct TweenOptions {
    // Seconds one play takes. 0 or less lands on the goal at the first step.
    double time = 1.0;
    // Seconds before the first play starts.
    double delay = 0.0;
    // Plays after the first. -1 repeats until cancelled.
    int repeat = 0;
    // Each play runs to the goal and then back to the start.
    bool reverses = false;
    anarchy::aanim::EasingStyle style = anarchy::aanim::EasingStyle::Quad;
    anarchy::aanim::EasingDirection direction = anarchy::aanim::EasingDirection::Out;
};

// Whether a value of this kind can tween: a Number, Vec2, Vec3, Color, or Matrix4.
bool tween_kind(LuaSlot::Kind kind);
// from at alpha 0 to to at alpha 1, exactly to at 1. A Matrix4's position and
// scale move linearly and its rotation along the shortest arc. False, leaving
// out alone, when the kinds differ or cannot tween. alpha may leave 0 to 1, as
// Back and Elastic do.
bool tween_lerp(const LuaSlot& from, const LuaSlot& to, double alpha, LuaSlot& out);

// One property a tween moves. from is read when the tween starts.
struct TweenGoal {
    const LuaField* field = nullptr;
    LuaSlot from;
    LuaSlot to;
};

class TweenWorld {
public:
    TweenWorld() = default;
    TweenWorld(const TweenWorld&) = delete;
    TweenWorld& operator=(const TweenWorld&) = delete;

    // Starts a tween of target's properties and returns its id, never 0. A
    // running tween that moves one of the same properties of target loses
    // that property; one left with none ends as cancelled.
    std::uint32_t start(DataModel& world, InstanceId target, std::vector<TweenGoal> goals, const TweenOptions& options);
    // Starts a tween of a plain value: each step fires stepped(id) with it.
    std::uint32_t start_value(LuaSlot from, LuaSlot to, const TweenOptions& options);

    // Each does nothing for an id that is gone or has ended.
    void pause(std::uint32_t id);
    void resume(std::uint32_t id);
    // Ends the tween where it is, as cancelled.
    void cancel(DataModel& world, std::uint32_t id);

    // The tween is still known: it runs, or a script still holds it.
    bool known(std::uint32_t id) const;
    // Running and not paused.
    bool playing(std::uint32_t id) const;
    bool ended(std::uint32_t id) const;
    // Whether an ended tween ran to its end; false when it was cancelled or replaced.
    bool completed(std::uint32_t id) const;

    // Fires once when the tween ends, with whether it ran to its end. Null for an unknown id.
    Signal* completed_signal(DataModel& world, std::uint32_t id);
    // A value tween's steps, each with the value. Null for an unknown id.
    Signal* stepped_signal(DataModel& world, std::uint32_t id);

    // A script's handle on the tween. An ended tween is forgotten once none holds it.
    void retain(std::uint32_t id);
    void release(DataModel& world, std::uint32_t id);

    // SimulationThread, once a play frame. A tween whose instance is gone ends
    // as cancelled, as does one whose instance refused a value: what it said is
    // returned, one line per refusal, for the console.
    std::vector<std::string> step(DataModel& world, double dt);
    // Forgets every tween, firing nothing: a Stop.
    void clear(DataModel& world);

    std::size_t size() const { return tweens_.size(); }

private:
    struct Tween {
        std::uint32_t id = 0;
        InstanceId target = 0;
        // The target's GUID, so an id reused by another instance is not written.
        std::string guid;
        std::vector<TweenGoal> goals;
        // A value tween's ends.
        bool plain = false;
        LuaSlot from;
        LuaSlot to;
        TweenOptions options;
        double delay_left = 0.0;
        // Seconds into this leg.
        double elapsed = 0.0;
        int repeats_left = 0;
        bool returning = false;
        bool paused = false;
        bool ended = false;
        bool completed = false;
        int holders = 0;
        std::unique_ptr<Signal> completed_signal;
        std::unique_ptr<Signal> stepped_signal;
    };

    Tween* find(std::uint32_t id);
    const Tween* find(std::uint32_t id) const;
    void finish(DataModel& world, Tween& tween, bool completed);
    // False when the target is gone or refused a value; a refusal is added to refusals.
    bool apply(DataModel& world, Tween& tween, double alpha, std::vector<std::string>& refusals);
    void forget(DataModel& world, std::uint32_t id);

    std::unordered_map<std::uint32_t, std::unique_ptr<Tween>> tweens_;
    // Running tweens, in the order they started.
    std::vector<std::uint32_t> running_;
    std::uint32_t next_id_ = 1;
};

}  // namespace engine_core
