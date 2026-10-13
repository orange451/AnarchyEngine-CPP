#pragma once

#include "Curve.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Slots of kind NumberCurve and ColorCurve holding a copy of curve.
LuaSlot number_curve_slot(NumberCurve curve);
LuaSlot color_curve_slot(ColorCurve curve);

// A NumberCurve as a project file holds it:
//   {"mode":"Constant","value":5}
//   {"mode":"Range","min":1,"max":3}
//   {"mode":"Curve","keys":[[time,value,"Linear"|"Smooth"|"Step"],...]}
//   {"mode":"CurveRange","keys":[...],"keys2":[...]}
JsonValue number_curve_to_json(const NumberCurve& curve);
// The reverse. nullopt, with error saying why, for anything else.
std::optional<NumberCurve> number_curve_from_json(const JsonValue& value, std::string& error);

// A ColorCurve as a project file holds it:
//   {"mode":"Constant","color":[r,g,b],"transparency":0}
//   {"mode":"RandomBetween","color":[...],"transparency":0,"color2":[...],"transparency2":0}
//   {"mode":"Gradient","keys":[[time,[r,g,b],transparency],...]}
//   {"mode":"GradientRange","keys":[...],"keys2":[...]}
JsonValue color_curve_to_json(const ColorCurve& curve);
std::optional<ColorCurve> color_curve_from_json(const JsonValue& value, std::string& error);

}  // namespace engine_core
