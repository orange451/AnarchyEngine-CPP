#pragma once

struct lua_State;

namespace engine_core {

// Roblox Vector3, installed into the same state as the rest of the script API.
// A Vector3 value is a Luau vector, so typeof is "Vector3", type is "vector",
// and + - * / // work on the value itself. vector.create returns the same value.
void open_vector3(lua_State* state);

}  // namespace engine_core
