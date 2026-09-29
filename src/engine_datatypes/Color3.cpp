#include "Color3.hpp"

#include "LuaApi.hpp"

#include "lualib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine_core {
namespace {

const char* kColor3Meta = "AE.Color3";

const Color3& check_color3(lua_State* state, int index) {
    const Color3* value = to_color3(state, index);
    if (value == nullptr) {
        luaL_typeerrorL(state, index, "Color3");
    }
    return *value;
}

// Omitted channels are 0. An explicit nil is not a number.
double channel_arg(lua_State* state, int index) { return lua_isnone(state, index) ? 0.0 : luaL_checknumber(state, index); }

float to_channel(double value) { return static_cast<float>(value); }

int color3_new(lua_State* state) {
    push_color3(state, Color3{to_channel(channel_arg(state, 1)), to_channel(channel_arg(state, 2)),
                              to_channel(channel_arg(state, 3))});
    return 1;
}

int color3_from_rgb(lua_State* state) {
    push_color3(state, Color3{to_channel(channel_arg(state, 1) / 255.0), to_channel(channel_arg(state, 2) / 255.0),
                              to_channel(channel_arg(state, 3) / 255.0)});
    return 1;
}

int color3_from_hsv_lua(lua_State* state) {
    push_color3(state, color3_from_hsv(luaL_checknumber(state, 1), luaL_checknumber(state, 2), luaL_checknumber(state, 3)));
    return 1;
}

int color3_from_hex_lua(lua_State* state) {
    Color3 color;
    if (!color3_from_hex(luaL_checkstring(state, 1), color)) {
        luaL_error(state, "Unable to convert characters to hex value");
    }
    push_color3(state, color);
    return 1;
}

// Color3.toHSV(color) and color:ToHSV() both return h, s, v.
int color3_to_hsv_lua(lua_State* state) {
    double hue = 0;
    double saturation = 0;
    double value = 0;
    color3_to_hsv(check_color3(state, 1), hue, saturation, value);
    lua_pushnumber(state, hue);
    lua_pushnumber(state, saturation);
    lua_pushnumber(state, value);
    return 3;
}

int color3_to_hex_lua(lua_State* state) {
    const std::string hex = color3_to_hex(check_color3(state, 1));
    lua_pushlstring(state, hex.data(), hex.size());
    return 1;
}

int color3_lerp(lua_State* state) {
    const Color3& from = check_color3(state, 1);
    const Color3& goal = check_color3(state, 2);
    const double alpha = luaL_checknumber(state, 3);
    auto blend = [alpha](float a, float b) { return alpha == 1.0 ? b : static_cast<float>(a + (b - a) * alpha); };
    push_color3(state, Color3{blend(from.r, goal.r), blend(from.g, goal.g), blend(from.b, goal.b)});
    return 1;
}

int color3_index(lua_State* state) {
    const Color3& value = check_color3(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (std::strcmp(name, "R") == 0) {
        lua_pushnumber(state, value.r);
        return 1;
    }
    if (std::strcmp(name, "G") == 0) {
        lua_pushnumber(state, value.g);
        return 1;
    }
    if (std::strcmp(name, "B") == 0) {
        lua_pushnumber(state, value.b);
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "%s is not a valid member of Color3", name);
}

int color3_newindex(lua_State* state) { luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2)); }

// "r, g, b", as Roblox prints a Color3.
int color3_tostring(lua_State* state) {
    const Color3& value = check_color3(state, 1);
    const float channels[] = {value.r, value.g, value.b};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            lua_pushliteral(state, ", ");
        }
        lua_pushnumber(state, channels[i]);
        luaL_tolstring(state, -1, nullptr);
        lua_remove(state, -2);
    }
    lua_concat(state, 5);
    return 1;
}

int color3_eq(lua_State* state) {
    const Color3* a = to_color3(state, 1);
    const Color3* b = to_color3(state, 2);
    lua_pushboolean(state, a != nullptr && b != nullptr && a->r == b->r && a->g == b->g && a->b == b->b ? 1 : 0);
    return 1;
}

void install_color3_metatable(lua_State* state) {
    luaL_newmetatable(state, kColor3Meta);

    lua_newtable(state);
    const luaL_Reg methods[] = {
        {"Lerp", color3_lerp}, {"ToHSV", color3_to_hsv_lua}, {"ToHex", color3_to_hex_lua}, {nullptr, nullptr}};
    for (const luaL_Reg* method = methods; method->func != nullptr; ++method) {
        lua_pushcfunction(state, method->func, method->name);
        lua_setfield(state, -2, method->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_pushcclosure(state, color3_index, "index", 1);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, color3_newindex, "__newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, color3_tostring, "__tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, color3_eq, "__eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "Color3");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
}

void install_color3_library(lua_State* state) {
    lua_newtable(state);
    const luaL_Reg constructors[] = {{"new", color3_new},
                                     {"fromRGB", color3_from_rgb},
                                     {"fromHSV", color3_from_hsv_lua},
                                     {"fromHex", color3_from_hex_lua},
                                     {"toHSV", color3_to_hsv_lua},
                                     {nullptr, nullptr}};
    for (const luaL_Reg* entry = constructors; entry->func != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Color3");
}

// Built here, not at namespace scope, for the reason Vector3.cpp gives.
ANARCHY_LUA_REGISTER(register_color3_lua) {
    const LuaField fields[] = {
        lua_property("R", "number", false, nullptr, nullptr), lua_property("G", "number", false, nullptr, nullptr),
        lua_property("B", "number", false, nullptr, nullptr), lua_method("Lerp", "Color3", nullptr),
        lua_method("ToHSV", "number", nullptr),               lua_method("ToHex", "string", nullptr),
    };
    register_lua_class("Color3", nullptr, fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    for (const char* constructor : {"new", "fromRGB", "fromHSV", "fromHex"}) {
        lua_note_result("Color3", constructor, "Color3", false);
    }
    LuaOperator equality;
    equality.metamethod = "__eq";
    equality.left = "Color3";
    equality.right = "Color3";
    equality.result = "boolean";
    register_lua_operators("Color3", &equality, 1);
}

int hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(digit)));
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

}  // namespace

Color3 color3_from_hsv(double hue, double saturation, double value) {
    // NaN or infinity has no place on the wheel, and would index outside the table below.
    if (!std::isfinite(hue)) {
        hue = 0.0;
    }
    const double h = (hue - std::floor(hue)) * 6.0;
    const double s = std::clamp(saturation, 0.0, 1.0);
    const double v = std::clamp(value, 0.0, 1.0);
    // h is in [0, 6] now, so this is a table row.
    const int sector = static_cast<int>(h) % 6;
    const double f = h - std::floor(h);
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    const double table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    return Color3{static_cast<float>(table[sector][0]), static_cast<float>(table[sector][1]),
                  static_cast<float>(table[sector][2])};
}

void color3_to_hsv(const Color3& color, double& hue, double& saturation, double& value) {
    const double high = std::max({color.r, color.g, color.b});
    const double range = high - std::min({color.r, color.g, color.b});
    value = high;
    saturation = high > 0.0 ? range / high : 0.0;
    hue = 0.0;
    if (range > 0.0) {
        if (high == color.r) {
            hue = (color.g - color.b) / range;
        } else if (high == color.g) {
            hue = 2.0 + (color.b - color.r) / range;
        } else {
            hue = 4.0 + (color.r - color.g) / range;
        }
        hue /= 6.0;
        if (hue < 0.0) {
            hue += 1.0;
        }
    }
}

std::string color3_to_hex(const Color3& color) {
    auto byte = [](float channel) { return static_cast<int>(std::lround(std::clamp(channel, 0.f, 1.f) * 255.f)); };
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X", byte(color.r), byte(color.g), byte(color.b));
    return buffer;
}

bool color3_from_hex(const std::string& text, Color3& color) {
    std::string digits = text;
    if (!digits.empty() && digits[0] == '#') {
        digits.erase(0, 1);
    }
    if (digits.size() != 3 && digits.size() != 6) {
        return false;
    }
    int values[6] = {};
    for (std::size_t i = 0; i < digits.size(); ++i) {
        values[i] = hex_digit(digits[i]);
        if (values[i] < 0) {
            return false;
        }
    }
    auto channel = [&](int index) {
        const int byte = digits.size() == 3 ? values[index] * 17 : values[index * 2] * 16 + values[index * 2 + 1];
        return static_cast<float>(byte / 255.0);
    };
    color = Color3{channel(0), channel(1), channel(2)};
    return true;
}

void push_color3(lua_State* state, Color3 value) {
    auto* data = static_cast<Color3*>(lua_newuserdata(state, sizeof(Color3)));
    *data = value;
    luaL_getmetatable(state, kColor3Meta);
    lua_setmetatable(state, -2);
}

const Color3* to_color3(lua_State* state, int index) {
    void* data = lua_touserdata(state, index);
    if (data == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, kColor3Meta);
    const bool match = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return match ? static_cast<const Color3*>(data) : nullptr;
}

void open_color3(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    install_color3_metatable(state);
    install_color3_library(state);
}

}  // namespace engine_core
