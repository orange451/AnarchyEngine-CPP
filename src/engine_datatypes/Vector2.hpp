#pragma once

struct lua_State;

namespace engine_core {

// A Vector2 on the C++ side.
struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

// Roblox Vector2, installed into the same state as the rest of the script API.
// Luau has no two-component vector, so a Vector2 is a userdata: typeof is
// "Vector2", type is "userdata", and + - * / // unary - and == work on it.
void open_vector2(lua_State* state);

// Pushes a new Vector2. open_vector2 must have run on the state.
void push_vector2(lua_State* state, Vec2 value);

// Null when the value at index is not a Vector2.
const Vec2* to_vector2(lua_State* state, int index);

}  // namespace engine_core
