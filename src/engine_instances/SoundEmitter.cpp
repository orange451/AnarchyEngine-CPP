#include "SoundEmitter.hpp"

#include "Contract.hpp"
#include "Enum.hpp"
#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot roll_off_slot(SoundEmitter::RollOff mode) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &roll_off_mode_enum();
    slot.number = static_cast<int>(mode);
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("SoundEmitter setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot SoundEmitter::sound() const { return instance_reference_slot(sound_ref_, "Sound"); }

InstanceId SoundEmitter::sound_id() const {
    const LuaSlot slot = sound();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

std::optional<std::string> SoundEmitter::set_sound(const LuaSlot& value) {
    require_thread(*this);
    // The audio world sees a new Sound by comparing it with the one its voice plays.
    std::optional<std::string> error = set_instance_reference("Sound", "Sound", sound_ref_, value);
    if (!error) {
        warned_file = false;
    }
    return error;
}

std::optional<std::string> SoundEmitter::set_number(const char* property, double& slot, double value, double max,
                                                    std::uint32_t dirty) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    value = std::clamp(value, 0.0, max);
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    dirty_ |= dirty;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> SoundEmitter::set_volume(double volume) {
    return set_number("Volume", volume_, volume, kMaxVolume, kDirtyMix);
}

std::optional<std::string> SoundEmitter::set_pitch(double pitch) {
    return set_number("Pitch", pitch_, pitch, kMaxPitch, kDirtyMix);
}

std::optional<std::string> SoundEmitter::set_roll_off_min_distance(double distance) {
    return set_number("RollOffMinDistance", roll_off_min_distance_, distance, kMaxRollOffDistance, kDirtyRollOff);
}

std::optional<std::string> SoundEmitter::set_roll_off_max_distance(double distance) {
    return set_number("RollOffMaxDistance", roll_off_max_distance_, distance, kMaxRollOffDistance, kDirtyRollOff);
}

std::optional<std::string> SoundEmitter::set_time_position(double seconds) {
    // No upper bound here: the audio world knows the sound's length, and a
    // seek past the end ends it.
    return set_number("TimePosition", time_position_, seconds, std::numeric_limits<double>::max(), kDirtySeek);
}

void SoundEmitter::set_looped(bool looped) {
    require_thread(*this);
    if (looped_ == looped) {
        return;
    }
    looped_ = looped;
    dirty_ |= kDirtyMix;
    note_property_change("Looped", bool_slot(!looped), bool_slot(looped));
}

std::optional<std::string> SoundEmitter::set_roll_off_mode(int mode) {
    require_thread(*this);
    if (enum_item_name(roll_off_mode_enum(), mode) == nullptr) {
        return std::string("RollOffMode must be an Enum.RollOffMode");
    }
    const RollOff next = static_cast<RollOff>(mode);
    if (next == roll_off_mode_) {
        return std::nullopt;
    }
    const RollOff previous = roll_off_mode_;
    roll_off_mode_ = next;
    dirty_ |= kDirtyRollOff;
    note_property_change("RollOffMode", roll_off_slot(previous), roll_off_slot(next));
    return std::nullopt;
}

void SoundEmitter::play() {
    require_thread(*this);
    if (playing_) {
        time_position_ = 0.0;
    }
    playing_ = true;
    ++play_serial_;
}

void SoundEmitter::stop() {
    require_thread(*this);
    playing_ = false;
    time_position_ = 0.0;
}

void SoundEmitter::store_playback(double time_position, bool playing) {
    time_position_ = time_position;
    playing_ = playing;
}

void SoundEmitter::read_place(const std::byte* data, std::size_t size) {
    DataModel::read_place(data, size);
    playing_ = false;
    dirty_ = kDirtyAll;
}

void SoundEmitter::on_reuse() {
    sound_ref_.set_guid(std::string());
    volume_ = kDefaultVolume;
    pitch_ = kDefaultPitch;
    looped_ = false;
    roll_off_mode_ = RollOff::Inverse;
    roll_off_min_distance_ = kDefaultRollOffMinDistance;
    roll_off_max_distance_ = kDefaultRollOffMaxDistance;
    time_position_ = 0.0;
    playing_ = false;
    play_serial_ = 0;
    dirty_ = kDirtyAll;
    warned_file = false;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

SoundEmitter* emitter_of(DataModel& object) { return dynamic_cast<SoundEmitter*>(&object); }

template <double (SoundEmitter::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    out = number_slot((emitter->*Get)());
    return true;
}

template <std::optional<std::string> (SoundEmitter::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    SoundEmitter* emitter = emitter_of(object);
    return emitter != nullptr && refuse(in, (emitter->*Set)(in.number));
}

bool read_sound(DataModel&, DataModel& object, LuaSlot& out) {
    const SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    out = emitter->sound();
    return true;
}

bool write_sound(DataModel&, DataModel& object, LuaSlot& in) {
    SoundEmitter* emitter = emitter_of(object);
    return emitter != nullptr && refuse(in, emitter->set_sound(in));
}

bool read_looped(DataModel&, DataModel& object, LuaSlot& out) {
    const SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    out = bool_slot(emitter->looped());
    return true;
}

bool write_looped(DataModel&, DataModel& object, LuaSlot& in) {
    SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    emitter->set_looped(in.flag);
    return true;
}

bool read_is_playing(DataModel&, DataModel& object, LuaSlot& out) {
    const SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    out = bool_slot(emitter->is_playing());
    return true;
}

bool read_roll_off_mode(DataModel&, DataModel& object, LuaSlot& out) {
    const SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    out = roll_off_slot(emitter->roll_off_mode());
    return true;
}

bool write_roll_off_mode(DataModel&, DataModel& object, LuaSlot& in) {
    SoundEmitter* emitter = emitter_of(object);
    if (emitter == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &roll_off_mode_enum()) {
        in.error = "RollOffMode must be an Enum.RollOffMode";
        return false;
    }
    return refuse(in, emitter->set_roll_off_mode(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

// Play and Stop are bound in ScriptBindings, which resolves self.
ANARCHY_LUA_REGISTER(register_sound_emitter_lua) {
    static const std::string volume = number_json(SoundEmitter::kDefaultVolume);
    static const std::string pitch = number_json(SoundEmitter::kDefaultPitch);
    static const std::string min_distance = number_json(SoundEmitter::kDefaultRollOffMinDistance);
    static const std::string max_distance = number_json(SoundEmitter::kDefaultRollOffMaxDistance);
    const LuaField fields[] = {
        lua_group("Playback"),
        lua_saved_property("Sound", "Sound?", read_sound, write_sound, "null"),
        lua_slider(lua_saved_property("Volume", "number", read_number<&SoundEmitter::volume>,
                                      write_number<&SoundEmitter::set_volume>, volume.c_str()),
                   0.0, SoundEmitter::kMaxVolume),
        lua_slider(lua_saved_property("Pitch", "number", read_number<&SoundEmitter::pitch>,
                                      write_number<&SoundEmitter::set_pitch>, pitch.c_str()),
                   0.0, SoundEmitter::kMaxPitch),
        lua_saved_property("Looped", "boolean", read_looped, write_looped, "false"),
        lua_saved_property("TimePosition", "number", read_number<&SoundEmitter::time_position>,
                           write_number<&SoundEmitter::set_time_position>, "0"),
        lua_property("IsPlaying", "boolean", false, read_is_playing, nullptr),
        lua_group("Roll-off"),
        lua_saved_enum("RollOffMode", roll_off_mode_enum(), read_roll_off_mode, write_roll_off_mode, "\"Inverse\""),
        lua_slider(lua_saved_property("RollOffMinDistance", "number", read_number<&SoundEmitter::roll_off_min_distance>,
                                      write_number<&SoundEmitter::set_roll_off_min_distance>, min_distance.c_str()),
                   0.0, SoundEmitter::kMaxRollOffDistance),
        lua_slider(lua_saved_property("RollOffMaxDistance", "number", read_number<&SoundEmitter::roll_off_max_distance>,
                                      write_number<&SoundEmitter::set_roll_off_max_distance>, max_distance.c_str()),
                   0.0, SoundEmitter::kMaxRollOffDistance),
    };
    register_lua_class("SoundEmitter", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("SoundEmitter", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
