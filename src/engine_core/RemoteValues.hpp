#pragma once

// A remote message's arguments: what RemoteEvent's Fire methods send and its
// handlers get (docs/superpowers/specs/2026-10-20-remote-events-design.md).

#include "LuaApi.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace engine_core {

class DataModel;

struct RemoteValue;

// A table's array part, 1..#t, and its string keys. Other keys are dropped.
struct RemoteTable {
    std::vector<RemoteValue> array;
    // In any order.
    std::vector<std::pair<std::string, RemoteValue>> fields;
};

// One value. nil, a boolean, a number and a string travel in slot (Nil, Bool,
// Number, String), as do engine values (Vec3, Vec2, Color, Matrix4, Enum,
// NumberCurve, ColorCurve) and instances (Instance, by id in this world). A
// table is in table.
struct RemoteValue {
    LuaSlot slot;
    // Non-null: this value is a table.
    std::shared_ptr<const RemoteTable> table;
};

// A fired payload: every argument, nils included, so values.size() is the
// sender's select("#", ...).
struct RemoteValues {
    std::vector<RemoteValue> values;
};

// Tables nest at most this deep: a table argument is level 1.
constexpr std::size_t kRemoteMaxDepth = 32;
// A reliable message's largest encoded size, header included.
constexpr std::size_t kRemoteMaxReliableBytes = 262144;
// A message's most values, counting every argument, every table, and every
// value inside one at any depth. A decoded value is a few hundred bytes, so a
// small message of many tiny values would otherwise unpack into a large one.
constexpr std::size_t kRemoteMaxValues = 65536;

// Reads stack[first..top] into out, every slot counted, nils included. False
// with error when a value cannot be sent: "<method> cannot send a <type>" for
// a function, a thread, a userdata the engine does not know, or a table that
// contains itself, "<method> tables nest deeper than 32", and "<method>
// message has more than 65536 values". An instance that is gone is sent as
// nil. Runs on the thread that holds the script's VM.
bool remote_values_from_lua(lua_State* state, int first, const char* method, RemoteValues& out, std::string& error);

// Pushes every value and returns how many. An Instance id no longer alive in
// world pushes nil. Tables arrive as new tables.
int push_remote_values(lua_State* state, const DataModel& world, const RemoteValues& values);

}  // namespace engine_core
