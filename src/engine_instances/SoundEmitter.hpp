#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// Plays a Sound. While the place plays and it is under game, the audio world
// (AudioWorld) plays it once Play is called, until Stop, the end of a sound
// that does not loop, or its destruction. When its parent is a PVInstance it
// is heard from that PVInstance's position, following it as it moves, and is
// quieter with distance as RollOffMode says; anywhere else it is 2D, the same
// in both ears at any distance.
//
// Sound               Sound?    what plays. Nil plays nothing.
// Volume              number    0.5, from 0 to kMaxVolume.
// Pitch               number    1, from 0 to kMaxPitch: the speed, which moves the pitch with it.
// Looped              boolean   false. Plays again from the start at its end.
// RollOffMode         Enum.RollOffMode  Inverse.
// RollOffMinDistance  number    10, from 0 to kMaxRollOffDistance: full volume within it.
// RollOffMaxDistance  number    100, from 0 to kMaxRollOffDistance: no quieter past it.
// TimePosition        number    seconds into the sound, not below 0. Play starts here.
// IsPlaying           boolean   read-only.
//
// Every property but IsPlaying is a saved registry property, so DataModel
// saves, loads, undoes, and restores it at Stop. A number out of range is
// clamped, and one that is not finite is refused. What the audio world writes
// back, TimePosition as the sound plays and IsPlaying at its end, goes through
// store_playback and fires no Changed.
class SoundEmitter : public DataModel {
public:
    // What a write changed, for the audio world to push into the voice.
    enum Dirty : std::uint32_t {
        kDirtyMix = 1u << 0,
        kDirtyRollOff = 1u << 1,
        kDirtySeek = 1u << 2,
        kDirtyAll = 0x7u,
    };

    enum class RollOff { Inverse = 0, Linear = 1, Exponential = 2, None = 3 };

    static constexpr double kDefaultVolume = 0.5;
    static constexpr double kMaxVolume = 5.0;
    static constexpr double kDefaultPitch = 1.0;
    static constexpr double kMaxPitch = 5.0;
    static constexpr double kDefaultRollOffMinDistance = 10.0;
    static constexpr double kDefaultRollOffMaxDistance = 100.0;
    static constexpr double kMaxRollOffDistance = 512.0;

    SoundEmitter(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "SoundEmitter"; }
    bool sound_source() const override { return true; }

    LuaSlot sound() const;
    // The live Sound, or 0.
    InstanceId sound_id() const;
    double volume() const { return volume_; }
    double pitch() const { return pitch_; }
    bool looped() const { return looped_; }
    RollOff roll_off_mode() const { return roll_off_mode_; }
    double roll_off_min_distance() const { return roll_off_min_distance_; }
    double roll_off_max_distance() const { return roll_off_max_distance_; }
    double time_position() const { return time_position_; }
    bool is_playing() const { return playing_; }
    // Moves on each Play, so the audio world restarts a voice Play was called on again.
    std::uint32_t play_serial() const { return play_serial_; }

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_sound(const LuaSlot& value);
    std::optional<std::string> set_volume(double volume);
    std::optional<std::string> set_pitch(double pitch);
    void set_looped(bool looped);
    std::optional<std::string> set_roll_off_mode(int mode);
    std::optional<std::string> set_roll_off_min_distance(double distance);
    std::optional<std::string> set_roll_off_max_distance(double distance);
    std::optional<std::string> set_time_position(double seconds);

    // SimulationThread. Play starts from TimePosition, or from 0 when it is
    // already playing. Stop ends it and puts TimePosition back to 0.
    void play();
    void stop();

    // The audio world's side. take_dirty returns what writes changed since
    // the last call and clears it. store_playback keeps where the voice is,
    // and whether it still plays, with no Changed, no history, and no dirty mark.
    std::uint32_t take_dirty() {
        const std::uint32_t out = dirty_;
        dirty_ = 0;
        return out;
    }
    void store_playback(double time_position, bool playing);

    // One warning until the Sound changes: its file could not be played.
    bool warned_file = false;

protected:
    void on_reuse() override;
    // Stop puts the saved properties back, as the base does, and stops it:
    // playing is play state, not place state.
    void read_place(const std::byte* data, std::size_t size) override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max,
                                          std::uint32_t dirty);

    InstanceRef sound_ref_;
    double volume_ = kDefaultVolume;
    double pitch_ = kDefaultPitch;
    bool looped_ = false;
    RollOff roll_off_mode_ = RollOff::Inverse;
    double roll_off_min_distance_ = kDefaultRollOffMinDistance;
    double roll_off_max_distance_ = kDefaultRollOffMaxDistance;
    double time_position_ = 0.0;
    bool playing_ = false;
    std::uint32_t play_serial_ = 0;
    std::uint32_t dirty_ = kDirtyAll;
};

}  // namespace engine_core
