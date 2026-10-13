#pragma once

#include "Curve.hpp"

#include <memory>

struct lua_State;

namespace engine_core {

// The NumberCurve and ColorCurve globals and their userdata.
void open_number_curve(lua_State* state);
void open_color_curve(lua_State* state);
void push_number_curve(lua_State* state, std::shared_ptr<const NumberCurve> curve);
void push_color_curve(lua_State* state, std::shared_ptr<const ColorCurve> curve);
// Null when the value at index is not one.
std::shared_ptr<const NumberCurve> to_number_curve(lua_State* state, int index);
std::shared_ptr<const ColorCurve> to_color_curve(lua_State* state, int index);

}  // namespace engine_core
