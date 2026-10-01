#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace engine_core {

class DataModel;

// The miniaudio engine behind SoundEmitter. Engine steps it once a frame
// while the place plays, after Heartbeat and the scripts, so what a script
// did this frame is heard this frame. Only this class's source includes
// miniaudio.
//
// Each step, on SimulationThread under the step lock:
//   1. A Stop since the last step (a new world_generation), or a place not
//      playing, drops every voice.
//   2. Every SoundEmitter under game that is playing has a voice, made from
//      its Sound's file the first time and again after each Play; one that
//      stopped, left game, or was destroyed, or whose Sound did, loses its
//      voice at once. A file that cannot be played warns once and stops the
//      SoundEmitter.
//   3. What scripts and Properties changed goes into the voice
//      (SoundEmitter::take_dirty).
//   4. An emitter whose parent is a PVInstance is heard from that
//      PVInstance's position, read again each step so the voice follows it.
//      Any other is 2D: not spatialized at all.
//   5. The listener is Workspace.CurrentCamera, looking down its -Z.
//   6. Each voice writes back TimePosition, and a voice that reached the end
//      of a sound that does not loop stops its SoundEmitter. These writes fire
//      no Changed and record no history.
//
// The output device is opened on the first voice, not before.
class AudioWorld {
public:
    // Without a device, nothing is heard: step mixes dt's worth of audio
    // itself, so tests run the same everywhere. A device that will not open
    // falls back to this, with a warning.
    explicit AudioWorld(bool device = true);
    ~AudioWorld();
    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    void step(DataModel& game, double dt);

    // Any thread. Silences every voice where it is, as a paused place is.
    // The next step plays on each that should still play.
    void suspend();

    // For tests: how many voices there are, and one SoundEmitter's.
    std::size_t voice_count() const;
    bool has_voice(InstanceId emitter) const;
    // Whether its voice is heard from a position, and that position.
    bool spatial(InstanceId emitter) const;
    Vec3 voice_position(InstanceId emitter) const;
    // Its voice's volume and pitch as miniaudio has them, or 0 without a voice.
    float voice_volume(InstanceId emitter) const;
    float voice_pitch(InstanceId emitter) const;
    // Whether its voice is sounding now, not stopped or suspended.
    bool voice_sounding(InstanceId emitter) const;
    // The listener's position.
    Vec3 listener_position() const;

    // Where a warning goes, such as a file that cannot be played. Unset, it
    // goes nowhere.
    void set_warning_sink(std::function<void(const std::string&)> sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine_core
