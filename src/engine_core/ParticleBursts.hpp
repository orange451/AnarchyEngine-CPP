#pragma once

#include "LuaApi.hpp"
#include "ParticleEmitter.hpp"
#include "PropertyBag.hpp"

#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace engine_core {

// ParticleEmitter.Bursts as a project file holds it:
//   [{"time":0,"count":{NumberCurve}}, ...]
JsonValue bursts_to_json(const std::vector<ParticleBurst>& bursts);
std::optional<std::vector<ParticleBurst>> bursts_from_json(const JsonValue& value, std::string& error);
// Why a list is refused, or nullopt: a Time below 0, or a Count that is not
// a Constant or a Range of 0 or more.
std::optional<std::string> check_bursts(const std::vector<ParticleBurst>& bursts);

// The property's slot: a String holding the list's JSON, so undo, history,
// and same_slot treat it as any string.
LuaSlot bursts_slot(const std::vector<ParticleBurst>& bursts);
std::optional<std::vector<ParticleBurst>> bursts_from_slot(const LuaSlot& slot, std::string& error);

// Scripts see an array of {Time = number, Count = NumberCurve}. A write takes
// Count as a number or a NumberCurve; the result is the JSON text for the slot.
void push_bursts(lua_State* state, const std::string& json_text);
bool read_bursts(lua_State* state, int index, std::string& json_text, std::string& error);

}  // namespace engine_core
