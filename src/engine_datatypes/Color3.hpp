#pragma once

#include <string>

struct lua_State;

namespace engine_core {

// A Color3 on the C++ side: red, green, and blue from 0 to 1, with no alpha.
struct Color3 {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

// Hue, saturation, and value from 0 to 1, as Roblox's Color3.fromHSV and ToHSV use them.
Color3 color3_from_hsv(double hue, double saturation, double value);
void color3_to_hsv(const Color3& color, double& hue, double& saturation, double& value);
// RRGGBB in uppercase without '#', as Roblox's ToHex returns it.
std::string color3_to_hex(const Color3& color);
// #RGB, #RRGGBB, or either without the '#'. False for anything else.
bool color3_from_hex(const std::string& text, Color3& color);

// Roblox Color3, installed into the same state as the rest of the script API.
// A Color3 is a userdata: typeof is "Color3", its channels are R, G, and B,
// and == compares them. new takes 0 to 1, fromRGB 0 to 255, fromHSV 0 to 1,
// and fromHex a hex code.
void open_color3(lua_State* state);

// Pushes a new Color3. open_color3 must have run on the state.
void push_color3(lua_State* state, Color3 value);

// Null when the value at index is not a Color3.
const Color3* to_color3(lua_State* state, int index);

}  // namespace engine_core
